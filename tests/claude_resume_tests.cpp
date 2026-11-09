#include "claude_accounts.h"
#include "claude_native_context_fixture.h"
#include "claude_preparation.h"
#include "claude_support.h"

#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/session_contracts.h>
#include <logonomic_cli_addon/sidecar.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
using Json = addon::Json_value;
using Availability = addon::Resume_availability;
using Content = addon::Resume_content;

constexpr const char* k_session = "a375d05e-f6cf-4812-80f5-91b33d24c053";
constexpr const char* k_user = "c8902599-6448-48be-9ca4-d67456950fae";
constexpr const char* k_assistant = "6ea1f7da-9d7c-4929-b0fb-f80f8fb74025";

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string host_path(const fs::path& path)
{
    const auto bytes = path.generic_u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

void write(const fs::path& path, std::string_view bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << bytes;
    stream.close();
    require(bool(stream), "fixture write: " + host_path(path));
}

std::string read(const fs::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    require(bool(stream), "fixture read: " + host_path(path));
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

std::map<std::string, std::string> snapshot(const fs::path& root)
{
    std::map<std::string, std::string> result;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        result.emplace(host_path(entry.path().lexically_relative(root)),
            entry.is_directory() ? "directory" : "file:" + read(entry.path()));
    }
    return result;
}

class Context : public addon::Invocation_context
{
public:
    const addon::Invoke_request& request() const override { return m_request; }
    bool cancel_requested() const override { return m_cancelled; }
    bool wait_for_cancel(int) override { return m_cancelled; }
    void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override {}
    void succeed(Json payload) override { m_payload = std::move(payload); m_finished = true; }
    void fail(addon::Result_status status, std::string reason, std::string) override
    {
        m_status = status;
        m_reason = std::move(reason);
        m_finished = true;
    }
    bool finished() const override { return m_finished; }

    addon::Invoke_request m_request;
    Json m_payload;
    addon::Result_status m_status = addon::Result_status::SUCCESS;
    std::string m_reason;
    bool m_cancelled = false;
    bool m_finished = false;
};

class Fixture
{
public:
    Fixture()
    :
        m_root(fs::temp_directory_path() / ("claude-resume-" + addon::random_nonce_hex()))
    {
        fs::create_directories(m_root / "work");
        m_root = fs::canonical(m_root);
        fs::create_directories(profile());
        fs::create_directories(m_root / "appdata");
#ifdef _WIN32
        const auto program = m_root / "package/bin/claude.exe";
        write(m_root / "package/package.json",
            R"({"name":"@anthropic-ai/claude-code","bin":{"claude":"bin/claude.exe"}})");
        write(program, "MZfixture");
        write(m_root / "package/bin/node.exe", "");
#else
        const auto program = m_root / "package/bin/claude";
        write(m_root / "package/package.json",
            R"({"name":"@anthropic-ai/claude-code","bin":{"claude":"bin/claude"}})");
        write(program, "\x7f" "ELFfixture");
        write(m_root / "package/bin/node", "");
        fs::permissions(program, fs::perms::owner_exec, fs::perm_options::add);
        fs::permissions(m_root / "package/bin/node", fs::perms::owner_exec, fs::perm_options::add);
#endif
        // These inert images are only inspected. Executing a native Claude
        // process would cross the fixture's account and model-call boundary.
        m_request.context.launcher = {"claude", "direct", ""};
        m_request.context.launcher_argv = {host_path(program)};
        m_request.context.working_directory = host_path(m_root / "work");
        addon::Launch_environment environment;
        environment.platform_id = addon::host_platform_id();
        environment.effective_environment = Json::object();
        environment.effective_environment.set("PATH", Json(host_path(program.parent_path())));
        environment.effective_environment.set("CLAUDE_CONFIG_DIR", Json(host_path(profile())));
        environment.effective_environment.set("HOME", Json(host_path(m_root / "home")));
        environment.effective_environment.set("USERPROFILE", Json(host_path(m_root / "home")));
        environment.effective_environment.set("APPDATA", Json(host_path(m_root / "appdata")));
        environment.effective_environment.set("XDG_CONFIG_HOME", Json(host_path(m_root / "appdata")));
        m_request.context.launch_environment = std::move(environment);
        m_request.resume_record = Json::object();
        m_request.resume_record.set("provider_session_id", Json(k_session));
    }

    ~Fixture()
    {
        std::error_code error;
        fs::remove_all(m_root, error);
    }

    fs::path profile() const { return m_root / "profile"; }
    const fs::path& root() const { return m_root; }
    addon::Resume_inspect_request& request() { return m_request; }
    Json& environment() { return m_request.context.launch_environment->effective_environment; }

    fs::path transcript(std::string project = {}) const
    {
        if (project.empty()) {
            project = m_request.context.working_directory;
            for (char& ch : project) {
                if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'))) {
                    ch = '-';
                }
            }
        }
        return profile() / "projects" / project / (std::string(k_session) + ".jsonl");
    }

    Context inspect(std::uint16_t version = 3) const
    {
        Context invocation;
        invocation.m_request.capability = {"interactive-resume", version};
        invocation.m_request.operation = "inspect_resume";
        invocation.m_request.payload = addon::encode_resume_inspect_request(m_request);
        claude::handle_preparation(invocation, m_root);
        return invocation;
    }

    void expect(Availability expected, const std::string& reason) const
    {
        const auto before = snapshot(m_root);
        const auto invocation = inspect();
        require(invocation.m_finished && invocation.m_status == addon::Result_status::SUCCESS,
            reason + ": inspection failed: " + invocation.m_reason);
        addon::Contract_failure failure;
        const auto result = addon::decode_resume_inspect_result(invocation.m_payload, &failure);
        require(result && result->availability == expected, reason + ": wrong availability");
        require(addon::validate_resume_inspection(m_request, *result, &failure),
            reason + ": result does not acknowledge the complete request");
        require(snapshot(m_root) == before, reason + ": inspection mutated fixture files");
    }

    void expect_content(Availability availability, Content content, const std::string& reason) const
    {
        const auto before = snapshot(m_root);
        for (const std::uint16_t version : {3, 4}) {
            const auto invocation = inspect(version);
            require(invocation.m_finished && invocation.m_status == addon::Result_status::SUCCESS,
                reason + ": inspection failed: " + invocation.m_reason);
            addon::Contract_failure failure;
            const auto result = addon::decode_resume_inspect_result(invocation.m_payload, &failure, version);
            require(result && result->availability == availability, reason + ": native availability changed");
            require(result->content == (version == 4 ? content : Content::UNKNOWN), reason + ": wrong content state");
            require(version == 4 || !invocation.m_payload.find("content"), reason + ": @3 result vocabulary changed");
            require(addon::validate_resume_inspection(m_request, *result, &failure),
                reason + ": result does not acknowledge the complete request");
        }
        require(snapshot(m_root) == before, reason + ": inspection mutated fixture files");
    }

    Context prepare(bool wake = false) const
    {
        addon::Resume_prepare_request request;
        request.context = m_request.context;
        request.resume_record = m_request.resume_record;
        request.title_instruction = "Set the session title.";
        if (wake) {
            request.context.profile_storage = addon::Profile_storage_context{1, host_path(m_root / "storage")};
            request.context.session_wake = addon::Session_wake_offer{};
        }
        Context invocation;
        invocation.m_request.capability = {"interactive-resume", 2};
        invocation.m_request.operation = "prepare_resume";
        invocation.m_request.payload = addon::encode_resume_prepare_request(request, 2);
        claude::handle_preparation(invocation, m_root);
        return invocation;
    }

private:
    fs::path m_root;
    addon::Resume_inspect_request m_request;
};

Json message(std::string_view type, const char* uuid, Json parent = Json())
{
    auto value = Json::object();
    value.set("type", Json(std::string(type)));
    value.set("uuid", Json(uuid));
    value.set("parentUuid", std::move(parent));
    value.set("sessionId", Json(k_session));
    value.set("isSidechain", Json(false));
    value.set("message", claude::object(R"({"content":"Retained conversation"})"));
    return value;
}

void expect_prepared(Fixture& fixture, const char* reason)
{
    const auto envelope = addon::serialize_json(addon::encode_resume_record(fixture.request().resume_record));
    const auto prepared = fixture.prepare();
    require(prepared.m_status == addon::Result_status::SUCCESS, std::string(reason) + ": preparation failed");
    addon::Contract_failure failure;
    const auto result = addon::decode_resume_prepare_result(prepared.m_payload, &failure, 2);
    require(result && addon::validate_resume_acknowledgment(
        fixture.request().resume_record, result->acknowledgment, &failure),
        std::string(reason) + ": exact record acknowledgment lost");
    require(result->process.argv.size() >= 2 &&
        result->process.argv[result->process.argv.size() - 2] == "--resume" &&
        result->process.argv.back() == k_session,
        std::string(reason) + ": exact native session selector lost");
    require(addon::serialize_json(addon::encode_resume_record(fixture.request().resume_record)) == envelope,
        std::string(reason) + ": stored opaque envelope changed");
}

const char* inspection_unavailable(const fs::path& working_directory)
{
#ifdef __APPLE__
    (void)working_directory;
    return "macOS managed-preference scope remains native";
#else
    return claude::test::native_context_unavailable(working_directory);
#endif
}

void native_context_contract()
{
    Fixture fixture;
    const auto* unavailable = inspection_unavailable(fixture.root());
    fixture.expect(unavailable ? Availability::UNKNOWN : Availability::ABSENT,
        "empty profile respects native policy authority");
    fixture.expect_content(unavailable ? Availability::UNKNOWN : Availability::ABSENT,
        unavailable ? Content::UNKNOWN : Content::EMPTY, "content inspection respects native policy authority");
    write(fixture.transcript(), addon::serialize_json(message("user", k_user)));
    fixture.expect(unavailable ? Availability::UNKNOWN : Availability::AVAILABLE,
        "saved transcript respects native policy authority");
    expect_prepared(fixture, "native policy preserves exact-identity resume preparation");
}

void transcript_contract()
{
    // Oracle: Claude's native loadConversationForResume searches its cwd,
    // worktrees and direct project children. Replay chooses a main user or
    // assistant leaf; compaction, rewinds and system-only fallback stay native.
    Fixture fixture;
    fixture.expect(Availability::ABSENT, "profile without projects has no saved conversation");
    const auto before = snapshot(fixture.root());
    const auto absent = fixture.prepare(true);
    require(absent.m_status == addon::Result_status::UNAVAILABLE && absent.m_reason == "conversation_absent",
        "definite absence refuses preparation before hooks or wake setup");
    require(snapshot(fixture.root()) == before, "absent preparation allocated hook or wake state");

    write(fixture.transcript(), "");
    fixture.expect(Availability::ABSENT, "empty transcript is not a conversation");
    const auto user = message("user", k_user);
    auto assistant = message("assistant", k_assistant, Json(k_user));
    assistant.find("message")->set("content", Json::array());
    const auto conversation = addon::serialize_json(user) + "\n" + addon::serialize_json(assistant);
    write(fixture.transcript(), conversation);
    fixture.expect(Availability::AVAILABLE, "main conversation survives an unterminated final JSONL line");
    expect_prepared(fixture, "available native conversation");

    std::vector<std::pair<std::string, std::string>> uncertain{
        {R"({"type":"summary","summary":"Retained title"})", "metadata alone does not prove resume"},
        {"{unfinished", "truncated JSON is uncertain"},
        {conversation + "\n{unfinished", "valid prefix cannot certify a corrupt tail"},
        {conversation + "\n" + R"({"type":"last-prompt","explicit":true})", "explicit rewind remains native"},
        {conversation + "\n" + R"({"type":"system","subtype":"compact_boundary"})", "compaction remains native"},
    };
    auto invalid = user;
    invalid.set("isSidechain", Json(true));
    uncertain.emplace_back(addon::serialize_json(invalid), "sidechain-only log is not a main conversation");
    invalid = user;
    invalid.set("sessionId", Json("f480fd10-a9dc-4df6-b311-0672d48b8c07"));
    uncertain.emplace_back(addon::serialize_json(invalid), "different session identity is uncertain");
    invalid = user;
    invalid.erase("parentUuid");
    uncertain.emplace_back(addon::serialize_json(invalid), "unknown message graph shape remains native");
    invalid = user;
    invalid.find("message")->erase("content");
    uncertain.emplace_back(addon::serialize_json(invalid), "message without native content is uncertain");
    for (const auto& [bytes, reason] : uncertain) {
        write(fixture.transcript(), bytes);
        fixture.expect(Availability::UNKNOWN, reason);
    }
    expect_prepared(fixture, "unknown transcript keeps native exact-identity confirmation");
}

void lookup_contract()
{
#ifndef __APPLE__
    Fixture fixture;
    const auto bytes = addon::serialize_json(message("user", k_user));
    write(fixture.transcript("other-worktree"), bytes);
    fixture.expect(Availability::AVAILABLE, "unique saved conversation in another project is resumable");
    write(fixture.transcript("second-project"), bytes);
    fixture.expect(Availability::UNKNOWN, "ambiguous global lookup cannot establish availability or absence");
    write(fixture.transcript(), bytes);
    fixture.expect(Availability::AVAILABLE, "current project takes priority over global duplicates");
    fs::remove(fixture.transcript());
    fs::remove(fixture.transcript("second-project"));
    fixture.environment().set("CLAUDE_CODE_PROJECT_DIR_NAME", Json("other-worktree"));
    fixture.expect(Availability::AVAILABLE, "project directory override retains the global unique lookup");
    fixture.environment().erase("CLAUDE_CODE_PROJECT_DIR_NAME");
    fs::remove(fixture.transcript("other-worktree"));
    write(fixture.profile() / "projects/other-worktree/subagents" / (std::string(k_session) + ".jsonl"), bytes);
    fixture.expect(Availability::ABSENT, "nested subagent log is not a native resume root");

    write(fixture.transcript(), bytes);
    fixture.environment().set("CLAUDE_CONFIG_DIR", Json("../profile"));
    fixture.expect(Availability::AVAILABLE, "relative profile resolves against requested working directory");
    fs::create_directories(fixture.root() / "home/.claude");
    fixture.environment().erase("CLAUDE_CONFIG_DIR");
    fixture.expect(Availability::ABSENT, "default profile does not borrow another profile's conversation");
    addon::Account_context account;
    account.version = 2;
    account.adapter_account_key = claude::account_key(fixture.profile());
    account.profile_path = host_path(fixture.profile());
    fixture.request().context.account_context = account;
    fixture.expect(Availability::AVAILABLE, "resolved account owns profile selection");
    fixture.request().context.account_context->adapter_account_key = "different-account";
    fixture.expect(Availability::UNKNOWN, "mismatched account identity cannot establish absence");
#endif
}

void content_contract()
{
    // Oracle: the Agent SDK session reader exposes main user/assistant
    // messages; its progress-only chain fixture returns no conversation.
    // Typed title/tag metadata is written separately by session_mutations.py.
    Fixture fixture;
    fixture.expect_content(Availability::ABSENT, Content::EMPTY, "missing conversation has no content");
    write(fixture.transcript(), "");
    fixture.expect_content(Availability::ABSENT, Content::EMPTY, "zero-byte conversation has no content");

    auto title = claude::object(R"({"type":"custom-title","customTitle":"Planning"})");
    title.set("sessionId", Json(k_session));
    const auto metadata = addon::serialize_json(title) + "\n";
    write(fixture.transcript(), metadata);
    fixture.expect_content(Availability::UNKNOWN, Content::EMPTY,
        "a retained title is independent of native availability and conversation content");
    expect_prepared(fixture, "empty content does not redefine native availability");
    auto tag = claude::object(R"({"type":"tag","tag":""})");
    tag.set("sessionId", Json(k_session));
    write(fixture.transcript(), metadata + addon::serialize_json(tag) + "\n" +
        R"({"type":"permission-mode","permissionMode":"acceptEdits"})" "\n");
    fixture.expect_content(Availability::UNKNOWN, Content::EMPTY, "native presentation metadata is not a conversation");

    auto system = message("system", k_user);
    system.erase("message");
    system.set("subtype", Json("turn_duration"));
    system.set("durationMs", Json(2400));
    auto progress = message("progress", k_assistant, Json(k_user));
    progress.erase("message");
    const auto system_rows = addon::serialize_json(system) + "\n" + addon::serialize_json(progress) + "\n";
    write(fixture.transcript(), system_rows);
    fixture.expect_content(Availability::UNKNOWN, Content::EMPTY,
        "a complete system and progress chain has no retained conversation messages");

    auto user = message("user", k_user);
    const auto user_row = addon::serialize_json(user);
    write(fixture.transcript(), user_row + "\n");
    fixture.expect_content(Availability::AVAILABLE, Content::NONEMPTY, "a main user message retains work");
    auto assistant = message("assistant", k_assistant, Json(k_user));
    assistant.find("message")->set("content", Json::array());
    write(fixture.transcript(), user_row + "\n" + addon::serialize_json(assistant) + "\n");
    fixture.expect_content(Availability::AVAILABLE, Content::NONEMPTY,
        "conversation content does not depend on the last assistant record's byte length");
    write(fixture.transcript(), user_row);
    fixture.expect_content(Availability::AVAILABLE, Content::UNKNOWN,
        "native resume accepts an unterminated record without certifying a complete content snapshot");

    std::vector<std::pair<std::string, std::string>> uncertain{
        {addon::serialize_json(title), "unterminated metadata can be an interrupted append"},
        {metadata + "{unfinished", "partial tail cannot certify empty content"},
        {metadata + R"({"type":"future-record"})" "\n", "unsupported native records remain unknown"},
        {R"({"type":"summary","summary":"Retained work"})" "\n", "summary-only replay remains native"},
        {system_rows + addon::serialize_json(system) + "\n", "repeated graph identities remain unknown"},
    };
    system.set("parentUuid", Json(k_assistant));
    uncertain.emplace_back(addon::serialize_json(system) + "\n", "missing ancestry cannot certify empty content");
    system.set("parentUuid", Json());
    system.set("subtype", Json("compact_boundary"));
    uncertain.emplace_back(addon::serialize_json(system) + "\n", "compaction replay remains native");
    user.set("isSidechain", Json(true));
    uncertain.emplace_back(addon::serialize_json(user) + "\n", "sidechain-only history remains unknown");
    title.set("sessionId", Json("f480fd10-a9dc-4df6-b311-0672d48b8c07"));
    uncertain.emplace_back(addon::serialize_json(title) + "\n", "metadata for another identity cannot prove emptiness");
    for (const auto& [bytes, reason] : uncertain) {
        write(fixture.transcript(), bytes);
        fixture.expect_content(Availability::UNKNOWN, Content::UNKNOWN, reason);
    }

    write(fixture.transcript(), metadata);
    fixture.expect_content(Availability::UNKNOWN, Content::EMPTY, "content inspection can observe empty metadata");
    {
        std::ofstream append(fixture.transcript(), std::ios::binary | std::ios::app);
        append << user_row.substr(0, user_row.size() / 2);
        require(bool(append), "partial late-write fixture appended");
    }
    fixture.expect_content(Availability::UNKNOWN, Content::UNKNOWN, "a late partial write replaces empty evidence");
    {
        std::ofstream append(fixture.transcript(), std::ios::binary | std::ios::app);
        append << user_row.substr(user_row.size() / 2) << '\n';
        require(bool(append), "completed late-write fixture appended");
    }
    fixture.expect_content(Availability::AVAILABLE, Content::NONEMPTY,
        "a later completed conversation returns without deleting or rewriting native history");
}

void context_contract()
{
#ifndef __APPLE__
    Fixture fixture;
    const auto retained = addon::serialize_json(message("assistant", k_assistant)) + '\n';
    write(fixture.transcript(), retained);
    // Transcript existence needs no credentials or quota authorization. Those
    // selectors remain owned by native launch and the separate quota contract.
    write(fixture.profile() / ".credentials.json", "not read by resume inspection");
    for (const auto* key : {"ANTHROPIC_API_KEY", "CLAUDE_CODE_OAUTH_TOKEN", "HTTPS_PROXY"}) {
        fixture.environment().set(key, Json("fixture-value"));
    }
    write(fixture.profile() / "settings.json", R"({"env":{"ANTHROPIC_API_KEY":"fixture-value"}})");
    fixture.expect(Availability::AVAILABLE, "resume inspection is independent of quota and authentication scope");

    write(fixture.profile() / "remote-settings.json", "{}");
#ifdef _WIN32
    const auto authentication = fixture.root() / "appdata/Anthropic";
#else
    const auto authentication = fixture.root() / "appdata/anthropic";
#endif
    write(authentication / "active_config", "fixture");
    write(authentication / "configs/fixture.json", "{}");
    write(authentication / "credentials/fixture.json", "not read by history inspection");
    fixture.expect_content(Availability::AVAILABLE, Content::NONEMPTY,
        "an empty remote settings cache and separate authentication store preserve retained history");
    fs::remove(fixture.transcript());
    fixture.expect_content(Availability::ABSENT, Content::EMPTY,
        "ordinary configuration and authentication files cannot make missing history uncertain");
    const auto before_absent = snapshot(fixture.root());
    const auto absent = fixture.prepare(true);
    require(absent.m_status == addon::Result_status::UNAVAILABLE && absent.m_reason == "conversation_absent",
        "known missing history with ordinary configuration refuses exact resume");
    require(snapshot(fixture.root()) == before_absent,
        "refused resume with ordinary configuration allocates no hook or wake state");
    auto title = claude::object(R"({"type":"custom-title","customTitle":"Planning"})");
    title.set("sessionId", Json(k_session));
    write(fixture.transcript(), addon::serialize_json(title) + '\n');
    fixture.expect_content(Availability::UNKNOWN, Content::EMPTY,
        "ordinary configuration also permits known-empty metadata inspection without redefining availability");
    write(fixture.transcript(), retained);

    for (const auto* key : {"CLAUDE_CODE_MANAGED_SETTINGS_PATH", "CLAUDE_CODE_REMOTE_SETTINGS_PATH",
        "CLAUDE_CODE_MOCK_REMOTE_SETTINGS", "USE_STAGING_OAUTH", "USE_LOCAL_OAUTH",
        "GIT_DIR", "GIT_WORK_TREE", "GIT_COMMON_DIR"})
    {
        fixture.environment().set(key, Json("fixture-selector"));
        fixture.expect(Availability::UNKNOWN, std::string("native location selector: ") + key);
        fixture.environment().erase(key);
    }
    for (const auto& path : {fixture.profile() / "settings.json", fixture.profile() / ".claude.json",
        fixture.profile() / "remote-settings.json",
        fixture.root() / "work/.claude/settings.local.json", fixture.root() / ".claude/settings.json"})
    {
        write(path, R"({"env":{"CLAUDE_CONFIG_DIR":"another-profile"}})");
        fixture.expect_content(Availability::UNKNOWN, Content::UNKNOWN,
            "settings redirect must retain native authority");
        write(path, "{unfinished");
        fixture.expect_content(Availability::UNKNOWN, Content::UNKNOWN,
            "malformed settings cannot prove unchanged lookup scope");
        fs::remove(path);
        fs::create_directory(path);
        fixture.expect_content(Availability::UNKNOWN, Content::UNKNOWN,
            "a settings source that cannot be read as a file retains native authority");
        fs::remove(path);
    }
    for (const auto& path : {fixture.profile() / ".config.json", fixture.root() / "work/.git"})
    {
        write(path, "{}");
        fixture.expect(Availability::UNKNOWN, "unresolved native settings or linked-worktree scope remains native");
        fs::remove(path);
    }
    const auto original = fixture.request().context.launcher_argv;
    for (const auto* option : {"--settings=profile.json", "--setting-sources=user"}) {
        fixture.request().context.launcher_argv.push_back(option);
        fixture.expect(Availability::UNKNOWN, "explicit settings selection remains native");
        fixture.request().context.launcher_argv = original;
    }
    fixture.request().context.launcher_argv = {"npx", "@anthropic-ai/claude-code"};
    fixture.expect(Availability::UNKNOWN, "package runner cannot prove native filesystem context");
    fixture.request().context.launcher_argv = original;
    write(addon::path_from_utf8(original.front()), "script wrapper");
    fixture.expect(Availability::UNKNOWN, "arbitrary selected executable is not bypassed");
    expect_prepared(fixture, "unsupported inspection context still permits exact native resume");
#endif
}

void inaccessible_transcript_contract()
{
#ifdef _WIN32
    Fixture fixture;
    const auto path = fixture.transcript();
    write(path, addon::serialize_json(message("user", k_user)));
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "fixture locks transcript against another reader");
    const auto invocation = fixture.inspect();
    CloseHandle(handle);
    const auto result = addon::decode_resume_inspect_result(invocation.m_payload, nullptr);
    require(result && result->availability == Availability::UNKNOWN,
        "inaccessible transcript cannot be reported absent");

    const auto remote_settings = fixture.profile() / "remote-settings.json";
    write(remote_settings, "{}");
    const HANDLE settings_handle = CreateFileW(
        remote_settings.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    require(settings_handle != INVALID_HANDLE_VALUE, "fixture locks remote settings against another reader");
    const auto settings_invocation = fixture.inspect(4);
    CloseHandle(settings_handle);
    const auto settings_result = addon::decode_resume_inspect_result(settings_invocation.m_payload, nullptr, 4);
    require(settings_result && settings_result->availability == Availability::UNKNOWN &&
        settings_result->content == Content::UNKNOWN,
        "unreadable remote settings cannot certify native availability or empty content");
#endif
}

void bounded_inspection_contract()
{
#ifndef __APPLE__
    Fixture fixture;
    const auto path = fixture.transcript();
    write(path, addon::serialize_json(message("user", k_user)) + "\n");
    {
        std::ofstream stream(path, std::ios::binary | std::ios::app);
        const std::string padding(1024 * 1024, ' ');
        for (int index = 0; index < 65; ++index) {
            stream << padding;
        }
        stream.close();
        require(bool(stream), "oversized valid transcript fixture written");
    }
    const auto size = fs::file_size(path);
    const auto modified = fs::last_write_time(path);
    const auto bounded_invocation = fixture.inspect(4);
    const auto result = addon::decode_resume_inspect_result(bounded_invocation.m_payload, nullptr, 4);
    require(result && result->availability == Availability::UNKNOWN && result->content == Content::UNKNOWN,
        "reaching the read budget cannot certify even a valid conversation prefix");
    require(fs::file_size(path) == size && fs::last_write_time(path) == modified,
        "bounded inspection leaves the source unchanged");
#endif
}

void cancelled_inspection_contract()
{
    Fixture cancelled;
    for (const std::uint16_t version : {3, 4}) {
        Context invocation;
        invocation.m_request.capability = {"interactive-resume", version};
        invocation.m_request.operation = "inspect_resume";
        invocation.m_request.payload = addon::encode_resume_inspect_request(cancelled.request());
        invocation.m_cancelled = true;
        claude::handle_preparation(invocation, cancelled.root());
        require(invocation.m_status == addon::Result_status::CANCELLED,
            "cancelled inspection never publishes availability or content");
    }
}

void packaged_contract(const fs::path& package)
{
    std::optional<Json> preparation_declaration;
    for (const auto version : {2, 3, 4}) {
        const auto sidecar = addon::parse_sidecar(
            read(package / addon::sidecar_relative_path("interactive-resume", (std::uint16_t)version)), nullptr);
        require(sidecar && sidecar->version == version, "packaged resume capability identity");
        if (preparation_declaration) {
            require(addon::serialize_json(*preparation_declaration) == addon::serialize_json(sidecar->payload),
                "inspection and preparation declare the same native scope selectors");
        }
        preparation_declaration = sidecar->payload;
    }
    Fixture fixture;
    addon::Simulator_options options;
    options.package_directory = package;
    options.validation.platform_id = addon::host_platform_id();
    options.scratch_directory = fixture.root() / "adapter-scratch";
    addon::Host_simulator host(options, nullptr);
    addon::Contract_failure failure;
    require(host.start(&failure), "packaged adapter starts: " + failure.reason_code + ": " + failure.detail);
    const auto live = host.live_capabilities();
    for (const auto version : {2, 3, 4}) {
        require(std::any_of(live.begin(), live.end(), [version](const auto& pair) {
            return pair.capability_id == "interactive-resume" && pair.version == version;
        }), "packaged adapter negotiates preparation and both inspection contracts");
    }
    auto request_id = host.invoke({"interactive-resume", 3}, "inspect_resume",
        addon::encode_resume_inspect_request(fixture.request()), 5000);
    auto outcome = host.wait_outcome(request_id, 6000);
    require(outcome && outcome->status == addon::Host_status::SUCCESS, "packaged inspection succeeds");
    const auto result = addon::decode_resume_inspect_result(outcome->payload, &failure);
    require(result && addon::validate_resume_inspection(fixture.request(), *result, &failure),
        "packaged inspection acknowledges the exact request");
    const auto expected = inspection_unavailable(fixture.root()) ? Availability::UNKNOWN : Availability::ABSENT;
    require(result->availability == expected, "packaged adapter respects native policy authority");
    require(!outcome->payload.find("content"), "packaged @3 response remains readable by an installed host");
    request_id = host.invoke({"interactive-resume", 4}, "inspect_resume",
        addon::encode_resume_inspect_request(fixture.request()), 5000);
    outcome = host.wait_outcome(request_id, 6000);
    require(outcome && outcome->status == addon::Host_status::SUCCESS, "packaged content inspection succeeds");
    const auto content_result = addon::decode_resume_inspect_result(outcome->payload, &failure, 4);
    require(content_result && content_result->availability == expected &&
        content_result->content == (expected == Availability::ABSENT ? Content::EMPTY : Content::UNKNOWN) &&
        addon::validate_resume_inspection(fixture.request(), *content_result, &failure),
        "packaged @4 content inspection acknowledges the same native scope");
    addon::Resume_prepare_request preparation;
    preparation.context = fixture.request().context;
    preparation.resume_record = fixture.request().resume_record;
    preparation.title_instruction = "Set the session title.";
    write(fixture.transcript(), addon::serialize_json(message("user", k_user)));
    request_id = host.invoke({"interactive-resume", 2}, "prepare_resume",
        addon::encode_resume_prepare_request(preparation, 2), 5000);
    outcome = host.wait_outcome(request_id, 6000);
    require(outcome && outcome->status == addon::Host_status::SUCCESS, "packaged @2 preparation remains available");
    const auto prepared = addon::decode_resume_prepare_result(outcome->payload, &failure, 2);
    require(prepared && addon::validate_resume_acknowledgment(
        preparation.resume_record, prepared->acknowledgment, &failure), "packaged preparation preserves opaque identity");
}

void installed_host_contract(const fs::path& package)
{
    Fixture fixture;
    addon::Simulator_options options;
    options.package_directory = package;
    options.validation.platform_id = addon::host_platform_id();
    options.scratch_directory = fixture.root() / "adapter-scratch";
    options.adopted_capabilities = std::vector<addon::Capability_pair>{
        {"interactive-resume", 2}, {"interactive-resume", 3}};
    addon::Host_simulator host(options, nullptr);
    addon::Contract_failure failure;
    require(host.start(&failure), "installed host starts an upgraded package: " + failure.detail);
    const auto live = host.live_capabilities();
    require(std::none_of(live.begin(), live.end(), [](const auto& pair) {
        return pair.capability_id == "interactive-resume" && pair.version == 4;
    }), "an installed host does not negotiate content inspection");
    const auto request_id = host.invoke({"interactive-resume", 3}, "inspect_resume",
        addon::encode_resume_inspect_request(fixture.request()), 5000);
    const auto outcome = host.wait_outcome(request_id, 6000);
    require(outcome && outcome->status == addon::Host_status::SUCCESS && !outcome->payload.find("content"),
        "an installed host receives the unchanged @3 result shape");
    const auto result = addon::decode_resume_inspect_result(outcome->payload, &failure, 3);
    const auto expected = inspection_unavailable(fixture.root()) ? Availability::UNKNOWN : Availability::ABSENT;
    require(result && result->availability == expected && result->content == Content::UNKNOWN &&
        addon::validate_resume_inspection(fixture.request(), *result, &failure),
        "an installed host retains native availability and the exact request acknowledgment");
}

} // namespace

int main(int argc, char** argv)
{
    try {
        require(argc == 2 || (argc == 3 && std::string_view(argv[2]) == "--filesystem"),
            "Expected the actual Claude package directory and optional --filesystem");
        if (argc == 3) {
            if (const auto* reason = inspection_unavailable(fs::temp_directory_path())) {
                std::cout << "Filesystem fixtures unavailable: " << reason << '\n';
                return 77;
            }
            transcript_contract();
            content_contract();
            lookup_contract();
            context_contract();
            inaccessible_transcript_contract();
            bounded_inspection_contract();
        }
        else {
            native_context_contract();
            cancelled_inspection_contract();
            packaged_contract(addon::path_from_utf8(argv[1]));
            installed_host_contract(addon::path_from_utf8(argv[1]));
        }
        std::cout << "Claude resume inspection and preparation contracts passed.\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
