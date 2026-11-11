#include "claude_resume.h"
#include "claude_accounts.h"
#include "claude_filesystem.h"
#include "claude_lifecycle.h"
#include "claude_launcher.h"
#include "claude_native_context.h"
#include "claude_support.h"

#include <logonomic_cli_addon/launcher_utilities.h>

#include <algorithm>
#include <array>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace claude {
namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
using Json = addon::Json_value;
using Availability = addon::Resume_availability;
using Content = addon::Resume_content;

// Inspection is advisory. Reaching a bound leaves the conversation unknown;
// it never proves that saved data is absent.
constexpr std::size_t k_max_inspection_bytes = 64 * 1024 * 1024;
constexpr std::size_t k_max_project_entries  = 16384;

bool transcript_location_environment(std::string_view name)
{
    const auto key = addon::lower(std::string(name));
    constexpr std::array<std::string_view, 10> selectors{
        "claude_config_dir", "claude_code_project_dir_name", "claude_code_managed_settings_path",
        "claude_code_remote_settings_path", "claude_code_mock_remote_settings",
        "use_staging_oauth", "use_local_oauth", "git_dir", "git_work_tree", "git_common_dir"};
    return std::find(selectors.begin(), selectors.end(), key) != selectors.end();
}

bool settings_preserve_transcript_location(const fs::path& path)
{
    const auto settings = native_context::read_object(path);
    if (!settings) {
        return false;
    }
    const auto* environment = settings->find("env");
    if (!environment) {
        return true;
    }
    if (!environment->is_object()) {
        return false;
    }
    for (const auto& entry : environment->as_object()) {
        if (transcript_location_environment(entry.name)) {
            return false;
        }
    }
    return true;
}

bool resume_context_supported(
    const addon::Session_launch_context& launch, const fs::path& profile,
    const std::vector<std::string>& command)
{
#ifdef __APPLE__
    // Native macOS managed preferences can redirect the profile without a
    // settings file. Leave that resolution to Claude until it can be read here.
    return false;
#endif
    const auto environment = addon::environment_pairs(launch.launch_environment->effective_environment);
    for (const auto& [name, value] : environment) {
        (void)value;
        const auto key = addon::lower(name);
        if (key != "claude_config_dir" && key != "claude_code_project_dir_name" &&
            transcript_location_environment(key))
        {
            return false;
        }
    }
    if (cli_argument_index(command) != 0) {
        return false;
    }
    for (std::size_t index = 1; index < command.size(); ++index) {
        const auto& argument = command[index];
        if (argument == "--settings" || argument.starts_with("--settings=") ||
            argument == "--setting-sources" || argument.starts_with("--setting-sources="))
        {
            return false;
        }
        index += claude_option_value_words(command, index);
    }
    const auto program = addon::resolve_program(command.front(), environment, launch.working_directory);
    if (!program || !native_context::selected_native_program(*program) || !native_context::policy_absent() ||
        !native_context::absent(profile / ".config.json") ||
        !settings_preserve_transcript_location(profile / "remote-settings.json") ||
        !settings_preserve_transcript_location(profile / "settings.json") ||
        !settings_preserve_transcript_location(profile / ".claude.json"))
    {
        return false;
    }
    const auto working_directory = addon::path_from_utf8(launch.working_directory);
    if (!native_context::absent(working_directory.root_path() / "home/claude/.claude/remote")) {
        return false;
    }
    // Settings env is applied before native resume. Inspect every ancestor so
    // Git-root selection cannot hide a profile redirect; linked-worktree and
    // managed/remote settings keep native authority when their scope is unclear.
    for (auto directory = working_directory; !directory.empty();) {
        if (!native_context::absent(directory / ".git")) {
            std::error_code error;
            if (!fs::is_directory(fs::symlink_status(native_io_path(directory / ".git"), error)) || error) {
                return false;
            }
        }
        for (const auto* name : {"settings.json", "settings.local.json"}) {
            if (!settings_preserve_transcript_location(directory / ".claude" / name)) {
                return false;
            }
        }
        const auto parent = directory.parent_path();
        if (parent == directory) {
            break;
        }
        directory = parent;
    }
    // The Anthropic SDK's separate configuration directory holds authentication
    // profiles, not Claude transcripts. Its presence does not redirect lookup.
    return true;
}

struct transcript_message_t
{
    bool conversation;
    bool sidechain;
};

class Content_inspection
{
public:
    explicit Content_inspection(std::string_view session_id)
    :
        m_session_id(session_id)
    {}

    void consume(const Json& entry)
    {
        if (m_unknown) {
            return;
        }
        const auto type = text(entry, "type");
        if (type == "custom-title" || type == "tag" || type == "permission-mode") {
            const char* field = type == "custom-title" ? "customTitle" :
                type == "tag" ? "tag" : "permissionMode";
            const auto* value = entry.find(field);
            const auto* id = entry.find("sessionId");
            m_unknown |= !value || !value->is_string() ||
                (type != "permission-mode" && !id) ||
                (id && (!id->is_string() || id->as_string() != m_session_id));
            return;
        }
        if (type != "user" && type != "assistant" && type != "system" && type != "progress") {
            m_unknown = true;
            return;
        }
        const auto uuid = text(entry, "uuid");
        const auto* parent = entry.find("parentUuid");
        if (text(entry, "sessionId") != m_session_id || !canonical_session_id(uuid) || !parent ||
            (!parent->is_null() && (!parent->is_string() || !m_seen.contains(parent->as_string()))) ||
            !m_seen.insert(uuid).second)
        {
            m_unknown = true;
            return;
        }
        // A complete append-only chain proves that no omitted parent carries
        // conversation content. Reordered, repeated and replay-specific rows
        // remain native authority rather than proving an empty conversation.
        for (const auto* field : {"isSidechain", "isMeta", "isCompactSummary"}) {
            if (const auto* value = entry.find(field); value && !value->is_boolean()) {
                m_unknown = true;
            }
        }
        if (const auto* team = entry.find("teamName"); team && !team->is_string()) {
            m_unknown = true;
        }
        if (const auto* logical_parent = entry.find("logicalParentUuid");
            logical_parent && !logical_parent->is_null())
        {
            m_unknown = true;
        }
        m_unknown |= text(entry, "subtype") == "compact_boundary" || flag(entry, "isCompactSummary") ||
            flag(entry, "isSidechain") || !text(entry, "teamName").empty();
        if (type == "user" || type == "assistant") {
            m_conversation = true;
            m_nonempty |= !flag(entry, "isSidechain") && !flag(entry, "isMeta") && text(entry, "teamName").empty();
        }
    }

    Content result() const
    {
        if (m_unknown) {
            return Content::UNKNOWN;
        }
        if (m_nonempty) {
            return Content::NONEMPTY;
        }
        return m_conversation ? Content::UNKNOWN : Content::EMPTY;
    }

private:
    std::string_view      m_session_id;
    std::set<std::string> m_seen;
    bool                 m_unknown = false;
    bool                 m_conversation = false;
    bool                 m_nonempty = false;
};

conversation_state_t inspect_transcript(
    const fs::path& path, std::string_view session_id, addon::Invocation_context& invocation)
{
    const auto io_path = native_io_path(path);
    std::ifstream stream(io_path, std::ios::binary);
    if (!stream) {
        return {};
    }
    std::error_code content_error;
    const auto size_before = fs::file_size(io_path, content_error);
    const bool size_known = !content_error;
    const auto modified_before = fs::last_write_time(io_path, content_error);
    const bool snapshot_known = size_known && !content_error;
    std::array<char, 16384> buffer;
    std::string line;
    std::size_t total = 0;
    std::map<std::string, transcript_message_t> messages;
    std::set<std::string> parents;
    Content_inspection content(session_id);
    bool uncertain = false;
    const auto consume = [&]() {
        if (line.find_first_not_of(" \t\r") == std::string::npos) {
            return;
        }
        addon::Json_parse_limits limits;
        limits.max_bytes        = k_max_inspection_bytes;
        limits.max_string_bytes = k_max_inspection_bytes;
        const auto entry = addon::parse_json(line, limits, nullptr);
        if (!entry || !entry->is_object()) {
            uncertain = true;
            return;
        }
        content.consume(*entry);
        const auto type = text(*entry, "type");
        if (type == "last-prompt" && (flag(*entry, "explicit") || flag(*entry, "rewound"))) {
            uncertain = true;
        }
        if (type != "user" && type != "assistant" && type != "system" && type != "attachment") {
            return;
        }
        // Native resume drops pre-compaction messages. Preserved segments and
        // explicit rewinds need its complete replay, so do not certify those.
        if (type == "system" && text(*entry, "subtype") == "compact_boundary") {
            uncertain = true;
        }
        const auto uuid = text(*entry, "uuid");
        const auto* parent = entry->find("parentUuid");
        if (!canonical_session_id(uuid) || !parent || (!parent->is_null() && !parent->is_string())) {
            uncertain = true;
            return;
        }
        const bool conversation = type == "user" || type == "assistant";
        if (conversation) {
            const auto* message = entry->find("message");
            const auto* content = message ? message->find("content") : nullptr;
            if (text(*entry, "sessionId") != session_id || !content ||
                (!content->is_string() && !content->is_array()))
            {
                uncertain = true;
                return;
            }
        }
        messages[uuid] = {conversation, flag(*entry, "isSidechain")};
        if (parent->is_string()) {
            parents.insert(parent->as_string());
        }
    };
    while (stream) {
        if (invocation.cancel_requested()) {
            return {};
        }
        stream.read(buffer.data(), buffer.size());
        const auto count = (std::size_t)stream.gcount();
        total += count;
        if (total > k_max_inspection_bytes) {
            return {};
        }
        for (std::size_t index = 0; index < count; ++index) {
            if (buffer[index] == '\n') {
                consume();
                line.clear();
            }
            else {
                line += buffer[index];
            }
        }
    }
    if (!stream.eof()) {
        return {};
    }
    consume();
    if (uncertain) {
        return {};
    }
    conversation_state_t result;
    // Native resume also understands system-only and explicitly cleared logs.
    // Only a genuinely empty transcript is definitely not a conversation.
    result.availability = total == 0 ? Availability::ABSENT : Availability::UNKNOWN;
    for (const auto& [uuid, message] : messages) {
        if (message.conversation && !message.sidechain && !parents.contains(uuid)) {
            result.availability = Availability::AVAILABLE;
            break;
        }
    }
    // @3 availability accepts a valid unterminated final record. Content needs
    // a complete snapshot so an interrupted or concurrent append stays unknown.
    const auto size_after = fs::file_size(io_path, content_error);
    const bool same_size = !content_error && size_after == size_before && total == size_before;
    const auto modified_after = fs::last_write_time(io_path, content_error);
    if (snapshot_known && same_size && !content_error && modified_after == modified_before &&
        line.find_first_not_of(" \t\r") == std::string::npos)
    {
        result.content = content.result();
    }
    return result;
}

std::string project_directory_name(const addon::Session_launch_context& launch)
{
    const auto environment = addon::environment_pairs(launch.launch_environment->effective_environment);
    const auto selected = addon::environment_value(environment, "CLAUDE_CODE_PROJECT_DIR_NAME");
    if (!selected.empty()) {
        // The native CLI validates this override. Unknown spellings must not
        // select a different directory here before the complete scope scan.
        return {};
    }
    std::string result = launch.working_directory;
    if (result.size() > 200 || std::any_of(result.begin(), result.end(), [](unsigned char ch) { return ch >= 128; })) {
        return {};
    }
    for (char& ch : result) {
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'))) {
            ch = '-';
        }
    }
    return result;
}

conversation_state_t inspect_profile(
    const addon::Session_launch_context& launch, std::string_view session_id,
    const std::vector<std::string>& command,
    addon::Invocation_context& invocation)
{
    const auto profile = profile_for_context(launch);
    const auto profile_text = path_string(profile);
    // Claude NFC-normalizes its config home. Without a shared Unicode
    // normalizer, an alternative spelling cannot establish absence.
    if (std::any_of(profile_text.begin(), profile_text.end(), [](unsigned char ch) { return ch >= 128; })) {
        return {};
    }
    if (!resume_context_supported(launch, profile, command)) {
        return {};
    }
    std::error_code error;
    if (!fs::is_directory(native_io_path(profile), error) || error ||
        !fs::is_directory(native_io_path(addon::path_from_utf8(launch.working_directory)), error) || error)
    {
        return {};
    }
    const auto projects = profile / "projects";
    const auto filename = std::string(session_id) + ".jsonl";
    const auto preferred = project_directory_name(launch);
    if (!preferred.empty()) {
        const auto direct = projects / preferred / filename;
        const auto status = fs::status(native_io_path(direct), error);
        if (error && error != std::errc::no_such_file_or_directory) {
            return {};
        }
        if (fs::is_regular_file(status)) {
            const auto state = inspect_transcript(direct, session_id, invocation);
            if (state.availability != Availability::ABSENT) {
                return state;
            }
        }
    }
    error.clear();
    fs::directory_iterator entry(native_io_path(projects), error), end;
    if (error == std::errc::no_such_file_or_directory) {
        return {Availability::ABSENT, Content::EMPTY};
    }
    if (error) {
        return {};
    }
    std::vector<fs::path> candidates;
    std::size_t visited = 0;
    // Claude --resume searches worktree project directories, then all direct
    // project children for a unique transcript. Subagent logs are not roots.
    for (; entry != end && !error; entry.increment(error)) {
        if (++visited > k_max_project_entries || invocation.cancel_requested()) {
            return {};
        }
        const auto directory_status = entry->symlink_status(error);
        if (error) {
            return {};
        }
        if (fs::is_symlink(directory_status)) {
            return {};
        }
        if (!fs::is_directory(directory_status)) {
            continue;
        }
        const auto candidate = projects / entry->path().filename() / filename;
        const auto status = fs::symlink_status(native_io_path(candidate), error);
        if (error == std::errc::no_such_file_or_directory) {
            error.clear();
            continue;
        }
        if (error || fs::is_symlink(status)) {
            return {};
        }
        if (fs::is_regular_file(status)) {
            candidates.push_back(candidate);
        }
    }
    if (error || candidates.size() > 1) {
        return {};
    }
    if (candidates.empty()) {
        return {Availability::ABSENT, Content::EMPTY};
    }
    return inspect_transcript(candidates.front(), session_id, invocation);
}

} // namespace

conversation_state_t inspect_saved_conversation(
    const addon::Session_launch_context& launch, std::string_view session_id,
    const std::vector<std::string>& command,
    addon::Invocation_context& invocation)
{
    try {
        return inspect_profile(launch, session_id, command, invocation);
    }
    catch (const std::exception&) {
        return {};
    }
}

} // namespace claude
