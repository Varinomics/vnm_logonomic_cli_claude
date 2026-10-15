#include "claude_accounts.h"
#include "claude_launcher.h"
#include "claude_lifecycle.h"
#include "claude_preparation.h"
#include "claude_probes.h"
#include "claude_sessions.h"
#include "claude_support.h"
#include "claude_usage.h"
#include "claude_wake.h"
#include <logonomic_cli_addon/sha256.h>

#include <logonomic_cli_addon/capability_ids.h>
#include <logonomic_cli_addon/legacy_state_import.h>
#include <logonomic_cli_addon/launch_preparation.h>
#include <logonomic_cli_addon/accounts.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/lifecycle_events.h>
#include <logonomic_cli_addon/limits.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/probe_contracts.h>
#include <logonomic_cli_addon/session_contracts.h>
#include <logonomic_cli_addon/session_wake.h>
#include <logonomic_cli_addon/sidecar.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace claude {
bool is_valid_claude_config_environment_text(std::string_view environment_text);
}

namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
using Json = addon::Json_value;

std::size_t unicode_scalar_count(std::string_view value)
{
    std::size_t count = 0;
    for (const unsigned char character : value) {
        if ((character & 0xC0) != 0x80) ++count;
    }
    return count;
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

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

// Oracle: how Claude itself reads its arguments. Claude parses them with
// commander's parseOptions: a word longer than one character that starts with
// '-' is an option; an option declared <value> takes the next word whatever it
// is, one declared [value] takes it unless it starts with '-', and one declared
// <values...> takes every word up to the next option; --name=value carries its
// value. The declarations are Claude Code 2.1's own (claude --help).
struct Claude_reading
{
    std::vector<std::pair<std::string, std::vector<std::string>>> options;
    std::vector<std::string>                                      operands;

    // The values of every occurrence of `name`, in order.
    std::vector<std::string> values(std::string_view name) const
    {
        std::vector<std::string> result;
        for (const auto& [option, option_values] : options) {
            if (option == name) result.insert(result.end(), option_values.begin(), option_values.end());
        }
        return result;
    }
};

Claude_reading claude_reading(const std::vector<std::string>& argv)
{
    const std::vector<std::string_view> required{
        "--agent", "--agents", "--append-system-prompt", "--autocompact", "--debug-file", "--effort",
        "--model", "-n", "--name", "--permission-mode", "--plugin-dir",
        "--remote-control-session-name-prefix", "--setting-sources", "--settings", "--system-prompt",
    };
    const std::vector<std::string_view> optional{"-d", "--debug", "--prompt-suggestions", "-r", "--resume"};
    const auto is_option = [](const std::string& word) { return word.size() > 1 && word.front() == '-'; };
    const auto declared = [](const std::vector<std::string_view>& names, std::string_view name) {
        return std::find(names.begin(), names.end(), name) != names.end();
    };
    Claude_reading reading;
    for (std::size_t index = 1; index < argv.size(); ++index) {
        const auto& word = argv[index];
        if (!is_option(word)) {
            reading.operands.push_back(word);
            continue;
        }
        const auto equals = word.find('=');
        const auto name   = word.substr(0, equals);
        std::vector<std::string> values;
        if (equals != std::string::npos) {
            values.push_back(word.substr(equals + 1));
        }
        else
        if (declared(required, name)) {
            if (index + 1 < argv.size()) values.push_back(argv[++index]);
        }
        else
        if (declared(optional, name)) {
            if (index + 1 < argv.size() && !is_option(argv[index + 1])) values.push_back(argv[++index]);
        }
        else
        if (name == "--add-dir" || name == "--allowedTools" || name == "--mcp-config" ||
            name == "--disallowedTools" || name == "--tools") {
            while (index + 1 < argv.size() && !is_option(argv[index + 1])) values.push_back(argv[++index]);
        }
        reading.options.emplace_back(name, std::move(values));
    }
    return reading;
}

Json protocol(bool review)
{
    auto result = claude::object(R"({"initialized":false,"pending_base64":"","result_base64":"","root_session_id":"","working_directory":"","expected_tools":["Read","Glob","Grep"],"task_tools":[],"expected_mcp_servers":[]})");
    if (review) result.set("working_directory", Json(claude::path_string(fs::current_path())));
    return result;
}

void launcher_contract()
{
    // Native recognition accepts executable/package identity, never a mention.
    require(claude::recognize_launcher("claude --model sonnet").has_value(), "direct Claude recognition");
    require(claude::recognize_launcher("npx --yes @anthropic-ai/claude-code@2.1.0").has_value(), "npx recognition");
    require(claude::recognize_launcher("bunx @anthropic-ai/claude-code").has_value(), "bunx recognition");
    require(!claude::recognize_launcher("echo claude"), "a mention is not a launcher");
    require(!claude::recognize_launcher("npx --package other @anthropic-ai/claude-code"), "unknown runner option");
    require(claude::interactive_resume_arguments({"claude", "--model", "sonnet"}), "stock resume flags");
    require(!claude::interactive_resume_arguments({"claude", "--resume", "already-selected"}), "resume cannot overwrite existing selector");
    require(!claude::interactive_resume_arguments({"claude", "-p", "existing prompt"}), "resume cannot steal print input");
    // Claude refuses to start an interactive session with prompt suggestions on.
    for (const auto& launcher : std::vector<std::vector<std::string>>{
            {"claude", "--prompt-suggestions"}, {"claude", "--prompt-suggestions", "true"},
            {"claude", "--prompt-suggestions", "--verbose"}, {"claude", "--prompt-suggestions=yes"}})
    {
        require(!claude::interactive_resume_arguments(launcher), "resume refuses a launcher with prompt suggestions on");
    }
    for (const auto& launcher : std::vector<std::vector<std::string>>{
            {"claude", "--prompt-suggestions", "Off", "--verbose"}, {"claude", "--prompt-suggestions=0"}})
    {
        require(claude::interactive_resume_arguments(launcher), "resume keeps a launcher with prompt suggestions off");
    }
    const std::string session = "a375d05e-f6cf-4812-80f5-91b33d24c053";
    const auto resumed = claude::prepared_argv({"claude", "--system-prompt", "--append-system-prompt"},
        "Logonomic instruction", {"C:/plugin root"}, {"--resume", session});
    const auto resumed_reading = claude_reading(*resumed);
    require(resumed_reading.operands.empty() &&
        resumed_reading.values("--system-prompt") == std::vector<std::string>{"--append-system-prompt"} &&
        resumed_reading.values("--append-system-prompt") == std::vector<std::string>{"Logonomic instruction"} &&
        resumed_reading.values("--plugin-dir") == std::vector<std::string>{"C:/plugin root"} &&
        resumed_reading.values("--resume") == std::vector<std::string>{session},
        "a resume keeps an option value that reads like an option, and adds no prompt");
    const auto prepared = claude::prepared_argv({"claude", "--append-system-prompt", "User instruction"},
        "Logonomic instruction", {"C:/plugin root"}, {});
    require(prepared && (*prepared)[2] == "User instruction Logonomic instruction", "preserve and append user instruction");
    require(prepared->at(prepared->size() - 2) == "--plugin-dir", "plugin flag appended");
    const std::vector<std::string> plugins{"C:/plugin root", "C:/wake plugin"};
    const auto both = claude::prepared_argv(*prepared, "Logonomic instruction", plugins, {});
    require(both && claude_reading(*both).values("--plugin-dir") == plugins,
        "preparation retains existing hooks and adds the negotiated wake plugin once");
    require(claude::prepared_argv(*both, "Logonomic instruction", plugins, {}) == both,
        "preparation does not duplicate either plugin directory");
    const auto shell = claude::prepared_argv({"powershell.exe", "-NoProfile", "-Command", "claude --model sonnet"},
        "Owner's instruction", {"C:/Owner's plugin"}, {});
    require(shell && (*shell)[1] == "-NoProfile" && shell->back().find("Owner''s") != std::string::npos,
        "PowerShell prefix and apostrophes preserved");
    require(!claude::prepared_argv({"bash", "-c", "claude; echo done"}, "Instruction", {}, {}),
        "control syntax is not rewritten");
    const std::string environment_prefix = "CLAUDE_CONFIG_DIR=";
    const std::string largest_path(addon::limits::k_max_environment_text_bytes - environment_prefix.size(), 'x');
    require(claude::is_valid_claude_config_environment_text(environment_prefix + largest_path),
        "Claude config environment text accepts the SDK byte limit");
    require(!claude::is_valid_claude_config_environment_text(environment_prefix + largest_path + "x"),
        "Claude config environment text rejects values over the SDK byte limit");
    require(!claude::is_valid_claude_config_environment_text(environment_prefix + std::string("bad\xFF", 4)),
        "Claude config environment text rejects invalid UTF-8");
}

void launcher_discovery_contract()
{
    const auto root = fs::temp_directory_path() / ("claude-discovery-" + addon::random_nonce_hex());
    const auto home = root / "home";
    const auto selected = home / ".claude";
    const auto sibling = home / ".claude-work";
#ifndef _WIN32
    const auto selected_alias = home / ".claude-alias";
    const auto tab_profile = home / ".claude-tab\tprofile";
#endif
#ifndef _WIN32
    const auto newline_profile = home / ".claude-newline\nCLAUDE_OTHER=owned";
#endif
    std::string long_profile_name = ".claude-long-";
    for (int i = 0; i < 60; ++i) long_profile_name += "\xC3\xA9";
    const auto long_profile = home / addon::path_from_utf8(long_profile_name);
    const auto bin = root / "bin";
    fs::create_directories(selected);
    fs::create_directories(sibling);
    fs::create_directories(long_profile);
#ifndef _WIN32
    fs::create_directories(newline_profile);
    fs::create_directories(tab_profile);
#endif
    fs::create_directories(bin);
    try {
#ifdef _WIN32
        const std::string platform = "windows-x86_64";
        const auto executable = bin / "claude.cmd";
#else
        const std::string platform = addon::host_platform_id();
        const auto executable = bin / "claude";
#endif
        std::ofstream(executable).close();
        std::ofstream(selected / ".credentials.json") << "{}";
        std::ofstream(sibling / ".credentials.json") << "{}";
        std::ofstream(long_profile / ".credentials.json") << "{}";
#ifndef _WIN32
        std::ofstream(newline_profile / ".credentials.json") << "{}";
        std::ofstream(tab_profile / ".credentials.json") << "{}";
        fs::create_directory_symlink(selected, selected_alias);
#endif

        addon::Discover_request request;
        request.platform_id = platform;
        request.ambient_environment = Json::object();
        request.ambient_environment.set("PATH", Json(claude::path_string(bin)));
#ifdef _WIN32
        request.ambient_environment.set("USERPROFILE", Json(claude::path_string(home)));
#else
        request.ambient_environment.set("HOME", Json(claude::path_string(home)));
#endif
        request.default_home_fallback = claude::path_string(home);

        const auto candidates = claude::discover_launchers(request);
#ifdef _WIN32
        require(candidates.size() == 3, "discovery returns every authenticated Claude profile");
#else
        require(candidates.size() == 4,
            "discovery rejects a profile path with a newline and retains the tab-named profile");
        require(std::none_of(candidates.begin(), candidates.end(), [](const auto& candidate) {
            return candidate.environment_text.find_first_of("\r\n") != std::string::npos;
        }), "profile paths cannot inject another line into launcher environment text");
        const auto tab_candidate = std::find_if(candidates.begin(), candidates.end(), [&](const auto& candidate) {
            return candidate.environment_text == "CLAUDE_CONFIG_DIR=" + claude::path_string(tab_profile);
        });
        require(tab_candidate != candidates.end() && std::none_of(
            tab_candidate->display_name.begin(), tab_candidate->display_name.end(), [](unsigned char character) {
                return character < 0x20 || character == 0x7f;
            }), "a tab in an authenticated profile name is sanitized from its display label");
        require(std::count_if(candidates.begin(), candidates.end(), [&](const auto& candidate) {
            return candidate.environment_text == "CLAUDE_CONFIG_DIR=" + claude::path_string(selected) ||
                candidate.environment_text == "CLAUDE_CONFIG_DIR=" + claude::path_string(selected_alias);
        }) == 1, "a symlink alias does not duplicate the selected Claude profile");
#endif
        const auto long_name_candidate = std::find_if(candidates.begin(), candidates.end(), [](const auto& candidate) {
            return candidate.environment_text.find("long-") != std::string::npos;
        });
        require(long_name_candidate != candidates.end() &&
            unicode_scalar_count(long_name_candidate->display_name) == addon::limits::k_max_display_name_scalars,
            "long Unicode profile labels are truncated at the display-name scalar limit");
        require(std::all_of(candidates.begin(), candidates.end(), [](const auto& candidate) {
            return unicode_scalar_count(candidate.display_name) <= addon::limits::k_max_display_name_scalars &&
                candidate.display_name.size() <= addon::limits::k_max_display_name_bytes;
        }), "all discovered profile labels fit the SDK display-name bounds");
        addon::Contract_failure failure;
        const auto decoded = addon::decode_discover_result(
            addon::encode_candidates(candidates), platform, &failure);
        require(decoded && decoded->size() == candidates.size(),
            "authenticated Claude candidates satisfy the public discovery contract");
        require(candidates[0].display_name == "Claude" && candidates[0].is_default &&
            candidates[0].environment_text == "CLAUDE_CONFIG_DIR=" + claude::path_string(selected),
            "the selected Claude profile is the valid default candidate");
        const auto sibling_candidate = std::find_if(candidates.begin(), candidates.end(), [&](const auto& candidate) {
            return candidate.environment_text == "CLAUDE_CONFIG_DIR=" + claude::path_string(sibling);
        });
        require(sibling_candidate != candidates.end() && sibling_candidate->display_name == "Claude work" &&
            !sibling_candidate->is_default,
            "a sibling Claude profile has its own valid launcher candidate");

        fs::remove(selected / ".credentials.json");
        const auto without_configured_credentials = claude::discover_launchers(request);
        require(!without_configured_credentials.empty() &&
            std::none_of(without_configured_credentials.begin(), without_configured_credentials.end(),
                [](const auto& candidate) { return candidate.is_default; }),
            "no sibling profile becomes default when the configured profile lacks credentials");
#ifdef _WIN32
        // A configured profile on a share that cannot be reached is not a
        // candidate; it must not fail the discovery of the Claude launcher.
        request.ambient_environment.set("CLAUDE_CONFIG_DIR", Json("\\\\localhost\\logonomic-absent-share\\"));
        const auto unreachable = claude::discover_launchers(request);
        require(unreachable.size() == 1 && unreachable.front().is_default &&
            unreachable.front().display_name == "Claude" && unreachable.front().environment_text.empty(),
            "a configured profile on an unreachable share leaves the plain Claude launcher discoverable");
#endif
    }
    catch (...) {
        std::error_code error;
        fs::remove_all(root, error);
        throw;
    }
    std::error_code error;
    fs::remove_all(root, error);
}

void launcher_discovery_candidate_limit()
{
    const auto root = fs::temp_directory_path() / ("claude-discovery-limit-" + addon::random_nonce_hex());
    const auto home = root / "home";
    const auto selected = home / ".claude";
    const auto bin = root / "bin";
    fs::create_directories(selected);
    fs::create_directories(bin);
    try {
#ifdef _WIN32
        const std::string platform = "windows-x86_64";
        std::ofstream(bin / "claude.cmd").close();
#else
        const std::string platform = addon::host_platform_id();
        std::ofstream(bin / "claude").close();
#endif
        std::ofstream(selected / ".credentials.json") << "{}";
        for (int i = 0; i < 24; ++i) {
            const auto profile = home / (".claude-profile-" + std::to_string(i));
            fs::create_directories(profile);
            std::ofstream(profile / ".credentials.json") << "{}";
        }

        addon::Discover_request request;
        request.platform_id = platform;
        request.ambient_environment = Json::object();
        request.ambient_environment.set("PATH", Json(claude::path_string(bin)));
#ifdef _WIN32
        request.ambient_environment.set("USERPROFILE", Json(claude::path_string(home)));
#else
        request.ambient_environment.set("HOME", Json(claude::path_string(home)));
#endif
        request.default_home_fallback = claude::path_string(home);

        const auto candidates = claude::discover_launchers(request);
        require(candidates.size() == addon::limits::k_max_discovery_candidates,
            "profile inventory is capped at the SDK discovery candidate limit");
        require(candidates.front().is_default &&
            candidates.front().environment_text == "CLAUDE_CONFIG_DIR=" + claude::path_string(selected),
            "the selected Claude profile is retained as the default at the candidate limit");
        addon::Contract_failure failure;
        const auto decoded = addon::decode_discover_result(
            addon::encode_candidates(candidates), platform, &failure);
        require(decoded && decoded->size() == addon::limits::k_max_discovery_candidates,
            "candidate-limited discovery satisfies the public SDK contract");
    }
    catch (...) {
        std::error_code error;
        fs::remove_all(root, error);
        throw;
    }
    std::error_code error;
    fs::remove_all(root, error);
}

void launcher_discovery_scan_limit()
{
    constexpr std::size_t scan_limit = 256;
    const auto root = fs::temp_directory_path() / ("claude-discovery-scan-limit-" + addon::random_nonce_hex());
    const auto home = root / "home";
    const auto selected = home / ".claude";
    const auto bin = root / "bin";
    fs::create_directories(selected);
    fs::create_directories(bin);
    try {
#ifdef _WIN32
        const std::string platform = "windows-x86_64";
        std::ofstream(bin / "claude.cmd").close();
#else
        const std::string platform = addon::host_platform_id();
        std::ofstream(bin / "claude").close();
#endif
        std::ofstream(selected / ".credentials.json") << "{}";
        for (std::size_t i = 0; i < scan_limit + 8; ++i) {
            fs::create_directories(home / (".claude-scan-" + std::to_string(i)));
        }

        std::vector<fs::path> directory_entries;
        for (const auto& entry : fs::directory_iterator(home)) directory_entries.push_back(entry.path());
        require(directory_entries.size() > scan_limit + 1,
            "the fixture has entries beyond the Claude profile scan bound");
        const auto configured_entry = std::find(directory_entries.begin(), directory_entries.end(), selected);
        require(configured_entry != directory_entries.end(), "the configured profile appears in its parent directory");
        auto target = directory_entries.begin() + static_cast<std::ptrdiff_t>(scan_limit);
        if (*target == selected) ++target;
        require(target != directory_entries.end() &&
            static_cast<std::size_t>(std::distance(directory_entries.begin(), target)) >= scan_limit,
            "the extra authenticated profile is positioned outside the inspected entries");
        std::ofstream(*target / ".credentials.json") << "{}";

        addon::Discover_request request;
        request.platform_id = platform;
        request.ambient_environment = Json::object();
        request.ambient_environment.set("PATH", Json(claude::path_string(bin)));
#ifdef _WIN32
        request.ambient_environment.set("USERPROFILE", Json(claude::path_string(home)));
#else
        request.ambient_environment.set("HOME", Json(claude::path_string(home)));
#endif
        request.default_home_fallback = claude::path_string(home);

        const auto candidates = claude::discover_launchers(request);
        require(candidates.size() == 1 && candidates.front().is_default &&
            candidates.front().environment_text == "CLAUDE_CONFIG_DIR=" + claude::path_string(selected),
            "discovery stops at the directory-entry bound and retains only the configured default");
    }
    catch (...) {
        std::error_code error;
        fs::remove_all(root, error);
        throw;
    }
    std::error_code error;
    fs::remove_all(root, error);
}

void launcher_directory_contract()
{
    const auto root = fs::temp_directory_path() / ("claude-path-" + addon::random_nonce_hex());
    fs::create_directories(root / "tools");
    try {
        addon::Launch_environment environment;
#ifdef _WIN32
        environment.platform_id = "windows-x86_64";
        const auto node = root / "tools/node.exe";
        const auto program = root / "tools/claude.exe";
#else
        environment.platform_id = addon::host_platform_id();
        const auto node = root / "tools/node";
        const auto program = root / "tools/claude";
#endif
        std::ofstream(node).close();
        std::ofstream(program).close();
        environment.effective_environment = Json::object();
        environment.effective_environment.set("PATH", Json("tools"));
        const auto directory_bytes = root.generic_u8string();
        const std::string directory(reinterpret_cast<const char*>(directory_bytes.data()), directory_bytes.size());
        require(claude::node_available(environment, directory), "relative Node PATH resolves against requested directory");
        require(!claude::node_available(environment, directory + "/missing"), "Node lookup never borrows adapter cwd");
        addon::Session_launch_context launch;
        launch.working_directory = directory;
        launch.launch_environment = environment;
        launch.launcher.script_body = "claude";
        launch.launcher.runner_key = "bash";
        launch.launcher_argv = {"bash", "-c", "claude"};
        const auto process = claude::cli_process(launch, false);
        require(!process.argv.empty() && addon::path_from_utf8(process.argv.front()) == program,
            "wrapped Claude relative PATH resolves against requested directory");
#ifdef _WIN32
        // Windows environment names are case-insensitive, and the host passes
        // the launcher environment in its own spelling: `Path` for the system
        // search path.
        auto windows_spelling = launch;
        auto& effective = windows_spelling.launch_environment->effective_environment;
        effective.erase("PATH");
        effective.set("Path", Json("tools"));
        const auto resolved = claude::cli_process(windows_spelling, false);
        require(!resolved.argv.empty() && addon::path_from_utf8(resolved.argv.front()) == program,
            "the Windows Path spelling resolves the wrapped Claude program");
        require(claude::node_available(*windows_spelling.launch_environment, directory),
            "the Windows Path spelling finds Node for hooks and resume");
#endif
        require(claude::canonical_resume_directory(directory), "clean absolute resume directory admitted");
        addon::Resume_prepare_request resume;
        resume.context = launch;
        resume.context.launch_environment->effective_environment.set(
            "CLAUDE_CONFIG_DIR", Json(claude::path_string(root / "selected-profile")));
        resume.title_instruction = "Set the session title.";
        resume.resume_record = claude::object(
            R"({"provider_session_id":"a375d05e-f6cf-4812-80f5-91b33d24c053"})");
        Context invocation;
        invocation.m_request.capability.capability_id = "interactive-resume";
        invocation.m_request.operation = "prepare_resume";
        invocation.m_request.payload = addon::encode_resume_prepare_request(resume, 2);
        claude::handle_preparation(invocation, root);
        require(invocation.m_finished && invocation.m_status == addon::Result_status::SUCCESS,
            "valid interactive resume reaches the actual producer");
        addon::Contract_failure failure;
        const auto resumed = addon::decode_resume_prepare_result(invocation.m_payload, &failure, 2);
        require(resumed && addon::validate_resume_process_proposal(resumed->process, &failure) &&
            addon::validate_launch_patch(resumed->patch, {}, {}, &failure),
            "produced resume satisfies host process and environment patch policy");
        require(resumed->process.environment.empty() && resumed->process.environment_names.empty() &&
            resumed->process.assets.empty(), "resume environment belongs exclusively to the launch patch");
        const std::vector<std::pair<std::string, std::string>> expected_environment{
            {"CLAUDE_CONFIG_DIR", claude::path_string(root / "selected-profile")},
            {"CLAUDE_CODE_SESSIONEND_HOOKS_TIMEOUT_MS", "5000"},
        };
        require(resumed->patch.set == expected_environment,
            "resume patch retains the frozen profile and hook timeout");
        require(!claude::canonical_resume_directory(directory + "/."), "dot resume directory refused");
        require(!claude::canonical_resume_directory(directory + "/tools/.."), "parent traversal resume directory refused");
        require(!claude::canonical_resume_directory(directory + "/"), "non-root trailing separator refused");
#ifdef _WIN32
        auto native = directory;
        for (char& ch : native) if (ch == '/') ch = '\\';
        require(!claude::canonical_resume_directory(native), "resume requires native oracle forward-separator spelling");
#endif
    }
    catch (...) {
        fs::remove_all(root);
        throw;
    }
    fs::remove_all(root);
}

// The actual launch-preparation producer for one launch, with the package at
// `root` and Node on the launch PATH.
Context prepare_launch(
    const fs::path&                                     root,
    const std::string&                                  platform,
    const std::string&                                  runner,
    const std::vector<std::string>&                     launcher_argv,
    const std::optional<addon::Initial_prompt_request>& initial_prompt,
    const std::string&                                  instruction)
{
    addon::Prepare_request request;
    request.launcher          = {"claude", runner, ""};
    request.working_directory = claude::path_string(root);
    request.title_instruction = instruction;
    addon::Launch_environment environment;
    environment.platform_id           = platform;
    environment.effective_environment = Json::object();
    environment.effective_environment.set("PATH", Json(claude::path_string(root / "tools")));
    request.launch_environment = environment;
    request.launcher_argv      = launcher_argv;
    request.initial_prompt     = initial_prompt;
    Context invocation;
    invocation.m_request.capability.capability_id = "launch-preparation";
    invocation.m_request.operation = "prepare";
    invocation.m_request.payload = addon::encode_prepare_request(request, 3);
    claude::handle_preparation(invocation, root);
    return invocation;
}

// The replacement argv of a prepared launch that the host admits as delivering
// `initial_prompt`.
std::vector<std::string> acknowledged_argv(const Context& invocation, const addon::Initial_prompt_request& initial_prompt)
{
    require(invocation.m_finished && invocation.m_status == addon::Result_status::SUCCESS,
        "a launcher that can carry the prompt is prepared");
    addon::Contract_failure failure;
    const auto patch = addon::decode_launch_patch(invocation.m_payload, &failure, 3);
    require(patch && addon::validate_launch_patch(*patch, {}, {}, &failure) &&
        addon::validate_initial_prompt_acknowledgment(initial_prompt, *patch, &failure),
        "the host admits the acknowledged launch");
    require(patch->disposition->prepared && patch->disposition->lifecycle_events,
        "the acknowledged launch is prepared and keeps its hooks");
    require(patch->initial_prompt->text_sha256_hex == addon::to_hex(addon::Sha256::digest(initial_prompt.text)),
        "the echo is SHA-256 over the exact prompt bytes");
    return *patch->argv;
}

// The host's continuation pointer to a brief.
std::string pointer(const std::string& brief)
{
    return "Logonomic continuation: read the brief at " + brief +
        " before anything else and follow it; do not modify the project until the user confirms.";
}

void require_refusal(const Context& invocation, const char* message)
{
    require(invocation.m_finished && invocation.m_status == addon::Result_status::UNAVAILABLE &&
        invocation.m_reason == "initial_prompt_unavailable" && invocation.m_payload.is_null(), message);
}

void initial_prompt_contract()
{
    const auto root = fs::temp_directory_path() / ("claude-prompt-" + addon::random_nonce_hex());
    fs::create_directories(root / "tools");
    try {
        // Both spellings of Node let either platform's rules run on any build host.
        std::ofstream(root / "tools/node").close();
        std::ofstream(root / "tools/node.exe").close();
        const std::string instruction = "Set the session title.";
        const auto plugin    = claude::path_string(root / "assets/claude-hooks");
        const auto directory = claude::path_string(root / "briefs/7f3c9a2e5b1d4f60");
        const auto brief     = claude::path_string(root / "briefs/7f3c9a2e5b1d4f60/brief.md");
        const addon::Initial_prompt_request prompt{pointer(brief), brief};
        const auto prepare = [
                &root,
                &instruction
            ](
                const std::string&                                  platform,
                const std::string&                                  runner,
                const std::vector<std::string>&                     launcher_argv,
                const std::optional<addon::Initial_prompt_request>& initial_prompt)
            {
                return prepare_launch(root, platform, runner, launcher_argv, initial_prompt, instruction);
            };

        const std::vector<std::string> expected{"claude", "--add-dir", directory, "--append-system-prompt",
            instruction, "--model", "sonnet", "--plugin-dir", plugin, prompt.text};
        for (const auto* platform : {"linux-x86_64", "windows-x86_64"}) {
            require(acknowledged_argv(prepare(platform, "direct", {"claude", "--model", "sonnet"}, prompt), prompt) ==
                expected, "the prompt is the one positional argument, last, behind a grant of only the brief's directory");
        }
        std::vector<std::string> spliced{"cmd.exe", "/c"};
        spliced.insert(spliced.end(), expected.begin(), expected.end());
        require(acknowledged_argv(prepare("windows-x86_64", "cmd", {"cmd.exe", "/c", "claude --model sonnet"}, prompt),
            prompt) == spliced, "cmd carries the same arguments as separate words");
        std::string quoted = "claude";
        for (auto argument = expected.begin() + 1; argument != expected.end(); ++argument) {
            quoted += " '" + *argument + "'";
        }
        const auto powershell = acknowledged_argv(prepare("windows-x86_64", "powershell",
            {"powershell.exe", "-NoLogo", "-Command", "claude --model sonnet"}, prompt), prompt);
        require(powershell.size() == 4 && powershell[3] == quoted, "PowerShell renders the same arguments quoted");
        const auto bash = acknowledged_argv(prepare("linux-x86_64", "bash",
            {"/usr/bin/bash", "-c", "claude --model sonnet"}, prompt), prompt);
        require(bash.size() == 3 && bash[2] == quoted, "a POSIX shell renders the same arguments quoted");

        const auto plain = prepare("linux-x86_64", "direct", {"claude", "--model", "sonnet"}, std::nullopt);
        addon::Contract_failure failure;
        const auto unprompted = addon::decode_launch_patch(plain.m_payload, &failure, 3);
        require(unprompted && !unprompted->initial_prompt && *unprompted->argv == std::vector<std::string>{
                "claude", "--append-system-prompt", instruction, "--model", "sonnet", "--plugin-dir", plugin},
            "a launch without an initial prompt gains neither a grant nor a prompt");

        for (const auto& launcher : std::vector<std::vector<std::string>>{
                {"npx", "@anthropic-ai/claude-code"}, {"claude", "--add-dir", directory}, {"claude", "-p"},
                {"claude", "--resume", "a375d05e-f6cf-4812-80f5-91b33d24c053"}, {"claude", "-c"},
                {"claude", "existing prompt"}})
        {
            require_refusal(prepare("linux-x86_64", "direct", launcher, prompt),
                "a package runner, an unknown option, print mode, a session selector or a prompt of its own refuses");
        }
        require_refusal(prepare("linux-x86_64", "bash", {"bash", "-c", "claude; echo done"}, prompt),
            "shell control syntax refuses");
        require_refusal(prepare("windows-x86_64", "bash", {"bash", "-lc", "claude"}, prompt),
            "a Windows POSIX shell rewrites path-like arguments and refuses");
        for (const auto* marker : {"%PATH%", "!PATH!", "\"quoted\""}) {
            const addon::Initial_prompt_request marked{pointer(brief) + " " + marker, brief};
            require_refusal(prepare("windows-x86_64", "cmd", {"cmd.exe", "/c", "claude"}, marked),
                "cmd.exe would rewrite a prompt with a variable reference or a quote");
            require(acknowledged_argv(prepare("linux-x86_64", "direct", {"claude"}, marked), marked).back() ==
                marked.text, "a POSIX launch carries every prompt byte");
        }
        require_refusal(prepare("windows-x86_64", "direct", {"claude", "--model", "sonnet&echo"}, prompt),
            "an unquoted Windows argument with a cmd operator refuses");
        require_refusal(prepare("windows-x86_64", "cmd", {"cmd.exe", "/c", "\"C:\\Program Files\\Claude\\claude.exe\""},
            prompt), "cmd.exe strips the quotes of a program with a space and refuses");
        require_refusal(prepare("windows-x86_64", "direct", {"C:\\Program Files\\nodejs\\claude.cmd"}, prompt),
            "cmd.exe also strips the quotes of a batch file with a space that the host starts directly");
        require(acknowledged_argv(prepare("windows-x86_64", "direct", {"C:\\Program Files\\Claude\\claude.exe"}, prompt),
            prompt).front() == "C:\\Program Files\\Claude\\claude.exe", "a native image with a space needs no cmd.exe");
        const std::string long_instruction(3900, 'i');
        const std::string long_prompt(3900, 's');
        require_refusal(prepare_launch(root, "windows-x86_64", "cmd",
                {"cmd.exe", "/c", "claude --system-prompt " + long_prompt}, prompt, long_instruction),
            "a command line over cmd.exe's limit refuses");
        require(acknowledged_argv(prepare_launch(root, "linux-x86_64", "direct",
                {"claude", "--system-prompt", long_prompt}, prompt, long_instruction), prompt).back() == prompt.text,
            "the same arguments fit a direct POSIX launch");
        require_refusal(prepare_launch(root, "linux-x86_64", "bash", {"bash", "-c", "claude"}, prompt, long_instruction),
            "a rendering over the argument bound refuses");
        require_refusal(prepare("windows-x86_64", "powershell", {"powershell.exe", "-NoLogo", "-Command", "claude"},
                addon::Initial_prompt_request{pointer(brief) + " O\xE2\x80\x99" "Brien", brief}),
            "PowerShell would end the quoted prompt at a typographic quote");
        require_refusal(prepare("windows-x86_64", "powershell",
                {"powershell.exe", "-NoLogo", "-Command", "claude --settings 'C:\\settings dir\\'"}, prompt),
            "Windows PowerShell would escape the closing quote after a trailing backslash");
        const auto top = claude::path_string(root.root_path() / "brief.md");
        require_refusal(prepare("linux-x86_64", "direct", {"claude"}, addon::Initial_prompt_request{pointer(top), top}),
            "a brief in a root directory would grant the root");
        const auto dotted = claude::path_string(root / "briefs/7f3c9a2e5b1d4f60/../brief.md");
        require_refusal(prepare("linux-x86_64", "direct", {"claude"},
            addon::Initial_prompt_request{pointer(dotted), dotted}), "a dotted path could name a wider directory");
        require_refusal(prepare("linux-x86_64", "direct", {"claude"},
            addon::Initial_prompt_request{"-" + prompt.text, brief}), "Claude would read a leading '-' as an option");
        require_refusal(prepare("linux-x86_64", "direct", {"claude"}, addon::Initial_prompt_request{"update", brief}),
            "Claude could read a single word as a subcommand");

        // Claude's own reading of each prepared launch: the prompt is its one
        // operand, the title instruction reaches --append-system-prompt, the
        // hook plugin and the brief's directory are the values of their
        // options, and every value the launcher gave stays that option's.
        for (const auto& launcher : std::vector<std::vector<std::string>>{
                {"claude", "--system-prompt", "--append-system-prompt"},
                {"claude", "--system-prompt", "--plugin-dir"},
                {"claude", "--name", "--verbose", "--append-system-prompt", "Keep answers short."},
                {"claude", "--prompt-suggestions", "off", "--model", "sonnet"},
                {"claude", "--prompt-suggestions=No"}})
        {
            for (const auto* platform : {"linux-x86_64", "windows-x86_64"}) {
                const auto reading = claude_reading(acknowledged_argv(prepare(platform, "direct", launcher, prompt), prompt));
                const auto appended = reading.values("--append-system-prompt");
                require(reading.operands == std::vector<std::string>{prompt.text} &&
                    appended.size() == 1 && appended.front().find(instruction) != std::string::npos &&
                    reading.values("--plugin-dir") == std::vector<std::string>{plugin} &&
                    reading.values("--add-dir") == std::vector<std::string>{directory},
                    "Claude reads the prompt as its one operand and each added value as its option's");
                const auto source = claude_reading(launcher);
                for (const auto& [option, values] : source.options) {
                    if (option != "--append-system-prompt" && option != "--plugin-dir") {
                        require(reading.values(option) == source.values(option), "every launcher value stays its option's");
                    }
                }
            }
        }
        {
            // Without Node there are no hooks, and no plugin to add.
            const auto bare = root / "without-node";
            fs::create_directories(bare);
            const auto launch = prepare_launch(bare, "linux-x86_64", "direct",
                {"claude", "--system-prompt", "--append-system-prompt"}, prompt, instruction);
            const auto patch = addon::decode_launch_patch(launch.m_payload, &failure, 3);
            require(launch.m_status == addon::Result_status::SUCCESS && patch &&
                addon::validate_initial_prompt_acknowledgment(prompt, *patch, &failure) &&
                !patch->disposition->lifecycle_events, "a launch without Node carries the prompt without hooks");
            const auto reading = claude_reading(*patch->argv);
            require(reading.operands == std::vector<std::string>{prompt.text} &&
                reading.values("--system-prompt") == std::vector<std::string>{"--append-system-prompt"} &&
                reading.values("--append-system-prompt") == std::vector<std::string>{instruction},
                "without hooks the title instruction is still an option value, not a second prompt");
        }

        // Claude submits its positional prompt as typed input: a prompt that
        // its input handling would change or not send is refused.
        for (const auto& text : std::vector<std::string>{
                "/review " + prompt.text, " " + prompt.text, prompt.text + " ", "exit ", "  ",
                prompt.text + " Then ultraplan the change.", prompt.text + " See [Pasted text #1 +4 lines].",
                "[Image #1] " + prompt.text, prompt.text + " [...Truncated text #2 +10 lines...]",
                prompt.text + "\xC2\xA0"})
        {
            require_refusal(prepare("linux-x86_64", "direct", {"claude"}, addon::Initial_prompt_request{text, brief}),
                "a prompt Claude would run as a command, trim, drop or rewrite refuses");
        }
        const addon::Initial_prompt_request bracketed{prompt.text + " Keep the plan [draft #1] as written.", brief};
        require(acknowledged_argv(prepare("linux-x86_64", "direct", {"claude"}, bracketed), bracketed).back() ==
            bracketed.text, "brackets that are no paste reference are carried");

        // A main-thread agent's own initial prompt comes first in the first
        // turn, and inline settings can select one or add a blocking hook.
        for (const auto& launcher : std::vector<std::vector<std::string>>{
                {"claude", "--agent", "reviewer"}, {"claude", "--agent=reviewer"},
                {"claude", "--settings", "{\"agent\":\"reviewer\"}"},
                {"claude", "--settings= {\"model\":\"sonnet\"} "},
                {"claude", "--settings", "\xC2\xA0{\"agent\":\"reviewer\"}\xE3\x80\x80"},
                {"claude", "--settings=\xEF\xBB\xBF\v{\"agent\":\"reviewer\"}\xE2\x80\xA8"}})
        {
            require_refusal(prepare("linux-x86_64", "direct", launcher, prompt),
                "a launcher that selects an agent or passes settings inline refuses");
        }
        for (const auto& launcher : std::vector<std::vector<std::string>>{
                {"claude", "--name", "--agent"}, {"claude", "--settings", "settings.json"},
                {"claude", "--settings=C:/Users/zoe/.claude/review-settings.json"}})
        {
            require(acknowledged_argv(prepare("linux-x86_64", "direct", launcher, prompt), prompt).back() == prompt.text,
                "a settings file, and an option value that reads like --agent, are the user's configuration");
        }
    }
    catch (...) {
        fs::remove_all(root);
        throw;
    }
    fs::remove_all(root);
}

void wake_preparation_contract()
{
    const auto root = fs::temp_directory_path() / ("claude-wake-preparation-" + addon::random_nonce_hex());
    fs::create_directories(root / "tools");
    try {
        addon::Prepare_request request;
        request.launcher          = {"claude", "direct", ""};
        request.launcher_argv     = {"claude"};
        const auto directory_bytes = root.generic_u8string();
        request.working_directory.assign(
            reinterpret_cast<const char*>(directory_bytes.data()), directory_bytes.size());
        request.title_instruction = "Set the session title.";
        request.profile_storage   = addon::Profile_storage_context{1, claude::path_string(root / "profile")};
        addon::Launch_environment environment;
#ifdef _WIN32
        environment.platform_id = "windows-x86_64";
        const auto node = root / "tools/node.exe";
#else
        environment.platform_id = addon::host_platform_id();
        const auto node = root / "tools/node";
#endif
        std::ofstream(node).close();
        environment.effective_environment = Json::object();
        environment.effective_environment.set("PATH", Json(claude::path_string(root / "tools")));
        environment.effective_environment.set("CLAUDE_CONFIG_DIR", Json(claude::path_string(root / "selected-profile")));
        request.launch_environment = environment;
        const auto prepare = [&]() {
            Context invocation;
            invocation.m_request.capability.capability_id = "launch-preparation";
            invocation.m_request.operation = "prepare";
            invocation.m_request.payload = addon::encode_prepare_request(request, 3);
            claude::handle_preparation(invocation, root);
            require(invocation.m_finished && invocation.m_status == addon::Result_status::SUCCESS,
                "wake preparation preserves ordinary launch success");
            const auto patch = addon::decode_launch_patch(invocation.m_payload, nullptr, 3);
            require(patch.has_value(), "wake preparation produces a decodable launch patch");
            return *patch;
        };
        const auto ordinary = prepare();
        const std::string hooks = claude::path_string(root / "assets/claude-hooks");
        const std::vector<std::string> plugins{hooks, claude::path_string(root / "assets/claude-wake-dispatch")};
        require(!ordinary.session_wake && ordinary.argv &&
            claude_reading(*ordinary.argv).values("--plugin-dir") == std::vector<std::string>{hooks},
            "ordinary launches keep regular hooks without negotiating wake");

        request.session_wake = addon::Session_wake_offer{};
        const auto offered = prepare();
        require(offered.session_wake && offered.session_wake->available && offered.argv &&
            claude_reading(*offered.argv).values("--plugin-dir") == plugins,
            "negotiated wake adds its native plugin alongside regular hooks");
        const auto directory = addon::path_from_utf8(claude::text(offered.session_wake->binding, "directory"));
        require(!fs::exists(directory) && fs::canonical(directory.parent_path()) == directory.parent_path() &&
            std::find(offered.set.begin(), offered.set.end(),
                std::pair(std::string("VNM_CLAUDE_WAKE_DIRECTORY"), claude::path_string(directory))) != offered.set.end(),
            "preparation reserves the same private path for binding and native plugin without allocating it");
#ifdef _WIN32
        const auto owner_helper = root / "helpers/claude_helper.exe";
#else
        const auto owner_helper = root / "helpers/claude_helper";
#endif
        require(std::find(offered.set.begin(), offered.set.end(),
            std::pair(std::string("VNM_CLAUDE_WAKE_OWNER_HELPER"), claude::path_string(fs::absolute(owner_helper)))) !=
                offered.set.end(), "the Mod receives the absolute packaged helper without a PATH-resolved launcher");
        for (int attempt = 0; attempt < 3; ++attempt) {
            const auto delayed = prepare();
            require(!fs::exists(addon::path_from_utf8(claude::text(delayed.session_wake->binding, "directory"))),
                "a discarded or delayed preparation does not allocate a wake directory");
        }
        require(fs::is_empty(directory.parent_path()), "unused preparations leave no per-launch artifacts");

        addon::Resume_prepare_request resume;
        resume.context.launcher          = request.launcher;
        resume.context.launcher_argv     = request.launcher_argv;
        resume.context.working_directory = request.working_directory;
        resume.context.launch_environment = request.launch_environment;
        resume.context.profile_storage   = request.profile_storage;
        resume.context.session_wake      = request.session_wake;
        resume.title_instruction = request.title_instruction;
        resume.resume_record = claude::object(
            R"({"provider_session_id":"a375d05e-f6cf-4812-80f5-91b33d24c053"})");
        Context invocation;
        invocation.m_request.capability.capability_id = "interactive-resume";
        invocation.m_request.operation = "prepare_resume";
        invocation.m_request.payload = addon::encode_resume_prepare_request(resume, 2);
        claude::handle_preparation(invocation, root);
        const auto resumed = addon::decode_resume_prepare_result(invocation.m_payload, nullptr, 2);
        require(invocation.m_status == addon::Result_status::SUCCESS && resumed &&
            resumed->patch.session_wake && resumed->patch.session_wake->available,
            "resume negotiates wake through its launch context");
        auto resumed_argv = resumed->process.argv;
        resumed_argv.insert(resumed_argv.begin(), "claude");
        const auto reading = claude_reading(resumed_argv);
        require(reading.values("--plugin-dir") == plugins && reading.operands.empty() &&
            reading.values("--resume") == std::vector<std::string>{"a375d05e-f6cf-4812-80f5-91b33d24c053"},
            "wake resume retains both plugins and the exact session selector without a prompt");

        request.profile_storage.reset();
        require(!prepare().session_wake->available, "wake without profile storage is unavailable");
        request.profile_storage = resume.context.profile_storage;
        request.launcher_argv = {"echo", "claude"};
        require(!prepare().session_wake->available, "an unsupported launcher cannot offer wake");
        request.launcher_argv = {"claude"};
        fs::remove(node);
        const auto no_node = prepare();
        require(!no_node.session_wake->available && no_node.argv &&
            claude_reading(*no_node.argv).values("--plugin-dir").empty(),
            "missing Node leaves launch available and wake unavailable");
    }
    catch (...) {
        fs::remove_all(root);
        throw;
    }
    fs::remove_all(root);
}

Context wake_invocation(const std::string& operation, Json payload)
{
    Context invocation;
    invocation.m_request.capability.capability_id = "session-wake";
    invocation.m_request.operation = operation;
    invocation.m_request.payload = std::move(payload);
    claude::handle_wake(invocation);
    return invocation;
}

void wake_binding_contract()
{
    const auto root = fs::temp_directory_path() / ("claude-wake-binding-" + addon::random_nonce_hex());
    fs::create_directories(root);
    try {
        addon::Session_wake_target target;
        target.provider_session_id = "a375d05e-f6cf-4812-80f5-91b33d24c053";
        target.working_directory   = claude::path_string(root);
        target.launcher_argv       = {"claude"};
        target.correlation.transition_nonce = addon::random_nonce_hex();
#ifdef _WIN32
        target.launch_environment.platform_id = "windows-x86_64";
#else
        target.launch_environment.platform_id = addon::host_platform_id();
#endif
        target.launch_environment.effective_environment = Json::object();
        target.binding.set("directory", Json(claude::path_string(fs::canonical(root))));
        const auto notify = [&]() {
            return addon::encode_session_wake_notify_request(
                {target, "notification-a", "Read the pending inbox message."});
        };
        const auto unavailable = [&]() {
            const auto inspected = wake_invocation("inspect", addon::encode_session_wake_inspect_request({target}));
            const auto readiness = addon::decode_session_wake_readiness(inspected.m_payload, nullptr);
            require(inspected.m_status == addon::Result_status::SUCCESS && readiness && !readiness->available,
                "a wake bridge without exact registration is not ready");
            const auto notified = wake_invocation("notify", notify());
            const auto result = addon::decode_session_wake_notify_result(notified.m_payload, nullptr);
            require(notified.m_status == addon::Result_status::SUCCESS && result &&
                result->disposition == addon::Session_wake_disposition::UNAVAILABLE,
                "a wake bridge without exact registration cannot submit");
            for (const auto& entry : fs::directory_iterator(root)) {
                require(entry.path().filename() == "registration.json",
                    "an unbound wake cannot publish a native request");
            }
        };
        unavailable();
        const auto register_target = [&](const std::string& session, const std::string& nonce) {
            Json registration = Json::object();
            registration.set("provider_session_id", Json(session));
            registration.set("transition_nonce", Json(nonce));
            std::ofstream(root / "registration.json", std::ios::binary) << addon::serialize_json(registration);
        };
        register_target("another-session", target.correlation.transition_nonce);
        unavailable();
        register_target(target.provider_session_id, addon::random_nonce_hex());
        unavailable();
        register_target(target.provider_session_id, target.correlation.transition_nonce);
        const auto valid_binding = target.binding;
        target.binding = Json::object();
        unavailable();
        target.binding.set("directory", Json("relative/bridge"));
        unavailable();
        target.binding.set("directory", Json(claude::path_string(root / "missing")));
        unavailable();
        target.binding = valid_binding;

        auto malformed = notify();
        malformed.erase("instruction");
        const auto refused = wake_invocation("notify", std::move(malformed));
        require(refused.m_status == addon::Result_status::FAILED && !refused.m_reason.empty(),
            "malformed wake requests fail before bridge submission");
        const auto bad_inspect = wake_invocation("inspect", Json::object());
        require(bad_inspect.m_status == addon::Result_status::FAILED,
            "inspect requires the full session and correlation contract");

        for (const std::string operation : {"inspect", "notify"}) {
            std::optional<Json> received;
            std::exception_ptr bridge_error;
            std::jthread bridge([&](std::stop_token stop) {
                try {
                    while (!stop.stop_requested()) {
                        for (const auto& entry : fs::directory_iterator(root)) {
                            if (entry.path().extension() != ".request") continue;
                            const auto pending = entry.path();
                            auto claimed = pending;
                            claimed.replace_extension(".claimed");
                            fs::rename(pending, claimed);
                            std::ifstream input(claimed, std::ios::binary);
                            const std::string bytes{
                                std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
                            received = addon::parse_json(bytes, addon::Json_parse_limits{}, nullptr);
                            const auto response = operation == "inspect"
                                ? addon::encode_session_wake_readiness({true, {}})
                                : addon::encode_session_wake_notify_result(
                                      {addon::Session_wake_disposition::ACCEPTED, {}, "native-receipt"});
                            auto writing = pending;
                            writing.replace_extension(".writing");
                            std::ofstream(writing, std::ios::binary) << addon::serialize_json(response);
                            auto destination = pending;
                            destination.replace_extension(".response");
                            fs::rename(writing, destination);
                            return;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    }
                }
                catch (...) {
                    bridge_error = std::current_exception();
                }
            });
            const auto invocation = wake_invocation(operation, operation == "inspect"
                ? addon::encode_session_wake_inspect_request({target}) : notify());
            bridge.request_stop();
            bridge.join();
            if (bridge_error) {
                std::rethrow_exception(bridge_error);
            }
            require(received && claude::text(*received, "operation") == operation,
                "matching session identity and nonce reach the native bridge");
            const auto* payload = received->find("payload");
            require(payload && payload->find("session") &&
                payload->find("session")->as_object().size() == 1 && !payload->find("binding"),
                "native wake requests exclude launcher environment and private host binding");
            if (operation == "inspect") {
                const auto readiness = addon::decode_session_wake_readiness(invocation.m_payload, nullptr);
                require(readiness && readiness->available, "native readiness is returned to the host");
            }
            else {
                const auto result = addon::decode_session_wake_notify_result(invocation.m_payload, nullptr);
                require(result && result->disposition == addon::Session_wake_disposition::ACCEPTED &&
                    result->receipt == "native-receipt", "native acceptance and receipt are preserved");
            }
        }
    }
    catch (...) {
        fs::remove_all(root);
        throw;
    }
    fs::remove_all(root);
}

void lifecycle_contract()
{
    {
        addon::Classify_event_request request;
        request.event.name = "ModTurnComplete";
        request.event.body = claude::object(
            R"({"hook_event_name":"ModTurnComplete","session_id":"native-session","reason":"answer"})");
        const auto result = claude::classify_event(request);
        require(result.classification == addon::Classification::EVENT && result.observations.size() == 1,
            "a native Mod completion publishes a root lifecycle event");
        const auto& observation = result.observations.front();
        require(observation.phase_transition == addon::Phase_transition::AWAITING_INPUT &&
            observation.phase_evidence == addon::Evidence::CONFIRMED && observation.provider_turn_id.empty() &&
            observation.outcome_transition == addon::Outcome_transition::NONE &&
            observation.attention_transition == addon::Attention_transition::NONE,
            "Mod completion confirms readiness without inventing turn outcome or attention");
        request.event.body.set("reason", Json("interrupted"));
        require(claude::classify_event(request).observations.empty(), "interruption does not confirm Mod readiness");
        request.event.body.set("reason", Json("answer"));
        request.event.body.set("agent_id", Json("child"));
        require(claude::classify_event(request).observations.empty(), "a child completion cannot confirm root readiness");
    }
    for (const auto& [tool_name, kind] : std::vector<std::pair<std::string, std::string>>{
            {"ExitPlanMode", "permission"}, {"AskUserQuestion", "interactive_question"}})
    {
        for (const std::string event_name : {"PreToolUse", "PostToolUse", "PostToolUseFailure"}) {
            addon::Attention_classification_request request;
            request.base.event.name = event_name;
            request.base.event.body = claude::object(R"({"session_id":"session","tool_use_id":"native-tool-request"})");
            request.base.event.body.set("hook_event_name", Json(event_name));
            request.base.event.body.set("tool_name", Json(tool_name));
            const auto result = claude::classify_attention(request);
            require(result.attention_kind && result.attention_kind->kind == kind &&
                result.attention_kind->observation_index == 0,
                "plan approval and interactive question retain distinct kinds on request and resolution");
            require(result.base.observations.size() == 1, "native tool event has one observation");
            const auto& observation = result.base.observations.front();
            require(observation.provider_event_name == event_name &&
                observation.attention_request_id == "native-tool-request",
                "attention classification preserves native event and paired tool identity");
            require(observation.attention_transition == (event_name == "PreToolUse"
                    ? addon::Attention_transition::REQUESTED : addon::Attention_transition::RESOLVED),
                "native pre and post events request and resolve the same attention");
        }
    }
    {
        addon::Attention_classification_request request;
        request.base.event.name = "Stop";
        request.base.event.body = claude::object(R"({"hook_event_name":"Stop","session_id":"session","last_assistant_message":"Which directory?"})");
        auto result = claude::classify_attention(request);
        require(result.attention_kind && result.attention_kind->kind == "conversational_hint",
            "ordinary complete assistant question gives inferred hint");
        const auto id = result.base.observations.front().attention_request_id;
        require(!id.empty() && result.base.observations.front().provider_event_name == "Stop",
            "hint has its own identity without rewriting native Stop");
        require(claude::classify_attention(request).base.observations.front().attention_request_id == id,
            "replayed event has stable hint identity");
        require(claude::classify_event(request.base).observations.front().attention_transition == addon::Attention_transition::NONE,
            "ordinary helper ingress does not acquire companion behavior");
        request.base.event.body.set("agent_id", Json("child"));
        require(!claude::classify_attention(request).attention_kind, "child message cannot raise root hint");
        request.base.event.name = "UserPromptSubmit";
        request.base.event.body = claude::object(R"({"hook_event_name":"UserPromptSubmit","session_id":"session"})");
        request.pending_conversational_request_id = id;
        result = claude::classify_attention(request);
        require(result.base.observations.front().attention_transition == addon::Attention_transition::RESOLVED &&
            result.base.observations.front().attention_request_id == id,
            "actual submission resolves only the host supplied conversational identity");
        request.pending_conversational_request_id.reset();
        require(!claude::classify_attention(request).attention_kind, "submission without pending hint invents no resolution");
    }
    addon::Classify_event_request request;
    request.event.name = "SessionStart";
    request.event.body = claude::object(R"({"hook_event_name":"SessionStart","session_id":"a375d05e-f6cf-4812-80f5-91b33d24c053","source":"resume"})");
    const auto start = claude::classify_event(request);
    require(start.classification == addon::Classification::TRANSITION && start.transition == addon::Transition_kind::RESUME,
        "native resume source selects resume transition");
    require(start.observations[0].resume_record.has_value(), "canonical Session UUID supplies exact resume record");
    request.event.name = "UserPromptSubmit";
    request.event.body = claude::object(R"({"hook_event_name":"UserPromptSubmit","session_id":"session","prompt_id":"turn"})");
    auto result = claude::classify_event(request);
    require(result.observations[0].phase_evidence == addon::Evidence::INFERRED, "Claude root phase evidence remains inferred");
    request.event.body.set("agent_id", Json("child"));
    require(claude::classify_event(request).classification == addon::Classification::IGNORED, "child root-looking event is ignored");
    request.event.name = "StopFailure";
    request.event.body = claude::object(R"({"hook_event_name":"StopFailure","session_id":"session","error":"provider failure"})");
    result = claude::classify_event(request);
    require(result.observations[0].outcome_transition == addon::Outcome_transition::NONE, "no invented turn correlation");
    request.event.body.set("prompt_id", Json("turn"));
    require(claude::classify_event(request).observations[0].outcome_transition == addon::Outcome_transition::FAILED,
        "native turn correlation authorizes failure");
}

// Oracle: the host's feature gate. It rejects an observation carrying a fact
// the package's own declaration leaves unsupported, phase and all.
void declared_profile_admits_every_shape(const fs::path& package)
{
    const addon::Directory_package_source source(package);
    const auto bytes = source.read(
        addon::sidecar_relative_path(addon::k_lifecycle_events_capability_id, addon::k_lifecycle_events_version),
        addon::limits::k_max_sidecar_bytes,
        nullptr);
    const auto sidecar = bytes ? addon::parse_sidecar(*bytes, nullptr) : std::nullopt;
    const auto declaration = sidecar ? addon::decode_lifecycle_declaration(sidecar->payload, nullptr) : std::nullopt;
    require(declaration.has_value(), "package lifecycle declaration is readable");
    addon::Attention_classification_request request;
    request.base.ledger.current_epoch = 1;
    const auto classify = [
            &request,
            &declaration
        ](
            const std::string& name,
            const std::string& members,
            const char*        shape)
        {
            request.base.event.name = name;
            request.base.event.body = claude::object(
                R"({"hook_event_name":")" + name + R"(","session_id":"a375d05e-f6cf-4812-80f5-91b33d24c053")" +
                members + "}");
            const auto result = claude::classify_attention(request);
            require(!result.base.observations.empty(), shape);
            for (const auto& observation : result.base.observations) {
                require(addon::lifecycle_profile_admits(declaration->features, observation), shape);
            }
            return result;
        };
    classify("SessionStart", R"(,"source":"startup")", "the declaration refuses a session start");
    classify("SessionStart", R"(,"source":"resume")", "the declaration refuses a session resume");
    classify("UserPromptSubmit", R"(,"prompt_id":"turn-a")", "the declaration refuses a turn start");
    classify("PermissionRequest", R"(,"prompt_id":"turn-a")", "the declaration refuses an approval request");
    classify("Stop", R"(,"prompt_id":"turn-a")", "the declaration refuses a turn completion");
    const auto hint = classify("Stop", R"(,"prompt_id":"turn-a","last_assistant_message":"Which directory?")",
        "the declaration refuses a completion that ends in a question");
    request.pending_conversational_request_id = hint.base.observations.front().attention_request_id;
    classify("UserPromptSubmit", R"(,"prompt_id":"turn-b")",
        "the declaration refuses the reply that resolves a conversational hint");
    request.pending_conversational_request_id.reset();
    classify("StopFailure", R"(,"prompt_id":"turn-b","error":"provider failure")",
        "the declaration refuses a failed turn");
    for (const std::string tool : {"AskUserQuestion", "ExitPlanMode"}) {
        for (const std::string name : {"PreToolUse", "PostToolUse", "PostToolUseFailure"}) {
            classify(name, R"(,"tool_use_id":"native-tool-request","tool_name":")" + tool + "\"",
                "the declaration refuses a structured question, plan approval or its answer");
        }
    }
    for (const std::string name : {"SubagentStart", "SubagentStop"}) {
        classify(name, R"(,"agent_id":"child-a","agent_type":"worker")",
            "the declaration refuses a subagent transition");
    }
    classify("SessionEnd", R"(,"reason":"exit")", "the declaration refuses a session end");
}

void structured_contract()
{
    addon::Structured_inspection input;
    input.protocol_state = addon::serialize_json(protocol(false));
    input.stdout_bytes = R"({"type":"system","subtype":"init","session_id":"root"})" "\n"
        R"({"type":"result","session_id":"child","subtype":"success","is_error":false,"result":"Child output"})" "\n"
        R"({"type":"result","session_id":"root","subtype":"success","is_error":false,"result":"Root output"})" "\n";
    auto result = claude::inspect_session(addon::Structured_purpose::IMPLEMENTATION, input);
    require(!result.complete && !result.result_bytes, "implementation waits for exit after root result");
    input.protocol_state = result.protocol_state;
    input.stdout_bytes.clear();
    input.final = true;
    result = claude::inspect_session(addon::Structured_purpose::IMPLEMENTATION, input);
    require(result.complete && result.result_bytes == "Root output", "child result cannot replace root result");
    input.exit_code = 1;
    require(claude::inspect_session(addon::Structured_purpose::IMPLEMENTATION, input).failure_reason.has_value(),
        "nonzero exit cannot authenticate result");
    input = {};
    input.protocol_state = addon::serialize_json(protocol(false));
    input.stdout_bytes = R"({"type":"system","subtype":"hook_started","session_id":"one"})" "\n"
        R"({"type":"system","subtype":"init","session_id":"two"})" "\n";
    require(claude::inspect_session(addon::Structured_purpose::IMPLEMENTATION, input).failure_reason.has_value(),
        "pre-init hook provenance must match root");
    auto init = claude::object(R"({"type":"system","subtype":"init","permissionMode":"plan","tools":["Grep","Read","Glob"],"mcp_servers":[],"plugins":[],"cwd":""})");
    init.set("cwd", Json(claude::path_string(fs::current_path())));
    init.set("session_id", Json("root"));
    input = {};
    input.protocol_state = addon::serialize_json(protocol(true));
    input.stdout_bytes = addon::serialize_json(init) + '\n';
    result = claude::inspect_session(addon::Structured_purpose::READ_ONLY_REVIEW, input);
    require(!result.complete && !result.failure_reason, "exact read-only surface admitted");
    const auto review_state = result.protocol_state;
    input.protocol_state = review_state;
    input.stdout_bytes = R"({"type":"result","session_id":"child","subtype":"success","is_error":false,"result":"Child output"})" "\n"
        R"({"type":"result","session_id":"root","parent_tool_use_id":"child-tool","subtype":"success","is_error":false,"result":"Nested output"})" "\n";
    result = claude::inspect_session(addon::Structured_purpose::READ_ONLY_REVIEW, input);
    require(!result.complete && !result.failure_reason && !result.result_bytes,
        "review child results cannot authenticate the root result");
    input.protocol_state = result.protocol_state;
    input.stdout_bytes = R"({"type":"result","session_id":"root","subtype":"success","is_error":false,"result":"Root review"})" "\n";
    result = claude::inspect_session(addon::Structured_purpose::READ_ONLY_REVIEW, input);
    require(!result.complete && !result.failure_reason && !result.result_bytes,
        "review retains the root result until final exit");
    input.protocol_state = result.protocol_state;
    input.stdout_bytes.clear();
    input.final = true;
    result = claude::inspect_session(addon::Structured_purpose::READ_ONLY_REVIEW, input);
    require(result.complete && result.result_bytes == "Root review",
        "review root identity survives incremental inspection");
    input.final = false;
    input.protocol_state = review_state;
    input.stdout_bytes = R"({"type":"result","subtype":"success","is_error":false,"result":"Unbound review"})" "\n";
    require(claude::inspect_session(addon::Structured_purpose::READ_ONLY_REVIEW, input).failure_reason ==
        "invalid_result_provenance", "review result requires native session identity");
    input.protocol_state = addon::serialize_json(protocol(true));
    init.set("session_id", Json(""));
    input.stdout_bytes = addon::serialize_json(init) + '\n';
    require(claude::inspect_session(addon::Structured_purpose::READ_ONLY_REVIEW, input).failure_reason ==
        "session_provenance_mismatch", "review initialization requires native session identity");
    init.set("session_id", Json("root"));
    init.find("tools")->push_back(Json("Write"));
    input.stdout_bytes = addon::serialize_json(init) + '\n';
    require(claude::inspect_session(addon::Structured_purpose::READ_ONLY_REVIEW, input).failure_reason.has_value(),
        "extra mutating tool invalidates read-only attestation");
}

addon::Delegated_prepare_result delegated_fixture(addon::Structured_purpose purpose)
{
    addon::Delegated_prepare_request request;
    request.purpose = purpose;
    request.prompt = "Inspect the selected task.\nReturn the findings.";
    request.context.launcher.script_body = "claude";
    request.context.launcher.runner_key = "direct";
    request.context.launcher_argv = {"claude"};
    request.context.working_directory = claude::path_string(fs::current_path());
    addon::Launch_environment environment;
#ifdef _WIN32
    environment.platform_id = "windows-x86_64";
#else
    environment.platform_id = addon::host_platform_id();
#endif
    environment.effective_environment = Json::object();
    environment.effective_environment.set("CLAUDE_CONFIG_DIR", Json(request.context.working_directory));
    request.context.launch_environment = environment;
    request.control.task_id = "readiness-task";
    request.control.private_artifact_root = claude::path_string(fs::current_path() / "private-readiness");
    request.control.connection = addon::Delegated_connection{
        claude::path_string(fs::current_path() / "host relay"),
        claude::path_string(fs::current_path() / "private-readiness/.logonomic/task-servers.json")};
    for (const auto* id : {"coordination", "delegation"}) {
        addon::Delegated_task_server server;
        server.server_id = id;
        server.program = claude::path_string(fs::current_path() / "task-control");
        server.environment = {{"VNM_CONTROL_TOKEN", "claude-host-authority-canary"}};
        server.actions = std::string_view(id) == "coordination" ?
            std::vector<std::string>{"status", "notice"} : std::vector<std::string>{"delegate", "inspect_task"};
        request.control.servers.push_back(std::move(server));
    }
    Context invocation;
    invocation.m_request.capability.capability_id = "delegated-session";
    invocation.m_request.operation = "prepare";
    invocation.m_request.payload = addon::encode_delegated_prepare_request(request, 3);
    claude::handle_sessions(invocation, fs::current_path());
    addon::Contract_failure failure;
    const auto prepared = addon::decode_delegated_prepare_result(invocation.m_payload, &failure, 3);
    require(prepared.has_value(), "delegated native preparation satisfies session@3 codec");
    require(prepared->acknowledgment.connection_version == addon::k_delegated_connection_version,
        "delegated preparation acknowledges the configured connection");
    const auto config = claude::object(prepared->private_artifacts.front().content);
    for (const auto& server : request.control.servers) {
        const auto* entry = config.find("mcpServers")->find("logonomic-task-readiness-task-" + server.server_id);
        require(entry && claude::text(*entry, "command") == request.control.connection->relay_program &&
            entry->find("env")->as_object().empty(), "MCP configuration uses the nonsecret relay");
        const auto& args = entry->find("args")->as_array();
        require(args.size() == 4 && args[0].as_string() == "--descriptor" &&
            args[1].as_string() == request.control.connection->descriptor_path &&
            args[2].as_string() == "--server" && args[3].as_string() == server.server_id,
            "MCP configuration preserves exact connection arguments");
    }
    require(prepared->private_artifacts.front().content.find("claude-host-authority-canary") == std::string::npos &&
        addon::serialize_json(invocation.m_payload).find("claude-host-authority-canary") == std::string::npos,
        "host credentials must not enter private artifacts or protocol results");
    auto disconnected = request;
    disconnected.control.connection.reset();
    Context refused;
    refused.m_request = invocation.m_request;
    refused.m_request.payload = addon::encode_delegated_prepare_request(disconnected, 3);
    claude::handle_sessions(refused, fs::current_path());
    require(refused.m_status == addon::Result_status::UNAVAILABLE &&
        refused.m_reason == "task_server_connection_unavailable", "missing connection refuses delegation locally");
    require(prepared->inspection.keep_stdin_open && prepared->inspection.incremental,
        "readiness retains native input across incremental inspection");
    require(prepared->acknowledgment.server_ids == std::vector<std::string>{"coordination", "delegation"},
        "readiness preserves host control acknowledgment");
    addon::Structured_prepare_request ordinary;
    ordinary.purpose = purpose;
    ordinary.context = request.context;
    ordinary.prompt = request.prompt;
    invocation.m_request.capability.capability_id = "structured-session";
    invocation.m_request.payload = addon::encode_structured_prepare_request(ordinary, 3);
    claude::handle_sessions(invocation, fs::current_path());
    const auto one_shot = addon::decode_structured_prepare_result(invocation.m_payload, &failure, 3);
    require(one_shot && !one_shot->inspection.keep_stdin_open &&
        one_shot->process.stdin_payload == request.prompt,
        "ordinary sessions retain raw one-shot input under session@3");
    return *prepared;
}

std::string control_reply(const std::string& state, Json payload = Json::object())
{
    Json response = Json::object();
    response.set("subtype", Json("success"));
    response.set("request_id", Json(claude::text(claude::object(state), "mcp_request_id")));
    response.set("response", std::move(payload));
    Json frame = Json::object();
    frame.set("type", Json("control_response"));
    frame.set("response", std::move(response));
    return addon::serialize_json(frame) + '\n';
}

addon::Structured_inspect_result inspect_readiness(
    addon::Structured_purpose purpose, const std::string& state, const std::string& output)
{
    addon::Structured_inspection inspection;
    inspection.protocol_state = state;
    inspection.stdout_bytes = output;
    auto result = claude::inspect_session(purpose, inspection);
    addon::Contract_failure failure;
    require(addon::decode_structured_inspect_result(
        addon::encode_structured_inspect_result(result, 3), false, &failure, 3).has_value(),
        "readiness output satisfies session@3 codec");
    return result;
}

void delegated_readiness_contract()
{
    for (const auto purpose : {addon::Structured_purpose::READ_ONLY_REVIEW, addon::Structured_purpose::IMPLEMENTATION}) {
        const auto prepared = delegated_fixture(purpose);
        const auto initial = prepared.inspection.protocol_state;
        const auto initialize = claude::object(prepared.process.stdin_payload);
        require(claude::text(initialize, "type") == "control_request" &&
            claude::text(*initialize.find("request"), "subtype") == "initialize" &&
            prepared.process.stdin_payload.find("Inspect the selected task") == std::string::npos,
            "native startup contains only initialize, never task bytes");
        const auto ack = control_reply(initial);
        auto result = inspect_readiness(purpose, initial, ack.substr(0, ack.size() / 2));
        require(!result.complete && !result.stdin_bytes && !result.close_stdin,
            "partial acknowledgment cannot advance readiness");
        result = inspect_readiness(purpose, result.protocol_state, ack.substr(ack.size() / 2));
        require(!result.complete && result.stdin_bytes && !result.close_stdin,
            "initialize acknowledgment starts awaited discovery without task release");
        require(claude::text(*claude::object(*result.stdin_bytes).find("request"), "subtype") == "mcp_status",
            "initial discovery is inspected through the live catalog snapshot");
        const auto status_state = result.protocol_state;
        auto catalog = claude::object(R"({"mcpServers":[{"name":"logonomic-task-readiness-task-coordination","status":"connected","tools":[{"name":"status"},{"name":"notice"},{"name":"extra"}]},{"name":"logonomic-task-readiness-task-delegation","status":"connected","tools":[{"name":"delegate"},{"name":"inspect_task"}]},{"name":"unrelated-native-server","status":"disabled"}]})");
        auto pending_catalog = catalog;
        auto& pending_server = pending_catalog.find("mcpServers")->as_array()[1];
        pending_server.set("status", Json("pending"));
        pending_server.erase("tools");
        const auto pending = inspect_readiness(purpose, status_state, control_reply(status_state, pending_catalog));
        require(!pending.complete && !pending.failure_reason && pending.stdin_bytes && !pending.close_stdin &&
            claude::text(*claude::object(*pending.stdin_bytes).find("request"), "subtype") == "mcp_status",
            "pending native discovery requests another catalog without releasing task input");
        const auto waiting = inspect_readiness(purpose, pending.protocol_state, "");
        require(!waiting.complete && !waiting.stdin_bytes && !waiting.close_stdin,
            "one catalog request stays outstanding until its response");
        require(claude::object(waiting.protocol_state).find("mcp_deadline_ms")->as_number() ==
            claude::object(status_state).find("mcp_deadline_ms")->as_number(),
            "pending discovery cannot extend the original readiness deadline");
        const auto discovered = inspect_readiness(purpose, waiting.protocol_state,
            control_reply(waiting.protocol_state, catalog));
        require(discovered.stdin_bytes && discovered.close_stdin && !discovered.failure_reason,
            "actual initial catalog after pending discovery admits task input");
        auto expired = claude::object(status_state);
        expired.set("mcp_deadline_ms", Json(0.0));
        const auto expired_state = addon::serialize_json(expired);
        for (const auto& snapshot : {pending_catalog, catalog}) {
            const auto timed_out = inspect_readiness(purpose, expired_state,
                control_reply(expired_state, snapshot));
            require(timed_out.failure_reason == "mcp_readiness_incomplete" && !timed_out.stdin_bytes,
                "elapsed readiness deadline refuses pending and late connected catalogs");
        }
        result = inspect_readiness(purpose, status_state, control_reply(status_state, catalog));
        require(!result.complete && !result.failure_reason && result.stdin_bytes && result.close_stdin,
            "positive catalog returns task input and ordered EOF in the same incomplete reply");
        const auto user = claude::object(*result.stdin_bytes);
        require(claude::text(user, "type") == "user" &&
            claude::text(*user.find("message"), "content") == "Inspect the selected task.\nReturn the findings.",
            "task is encoded as the exact native JSON user frame");
        require(!claude::flag(claude::object(result.protocol_state), "initialized"),
            "control acknowledgments do not fabricate native initialization");
        const auto released = result.protocol_state;
        auto early_tail = inspect_readiness(purpose, status_state,
            control_reply(status_state, catalog) + "{\"type\":");
        require(!early_tail.stdin_bytes && !early_tail.close_stdin,
            "an unclassified preprompt tail withholds task release");
        {
            require(!early_tail.complete && !early_tail.failure_reason &&
                claude::text(claude::object(early_tail.protocol_state), "mcp_phase") == "verified",
                "verified catalog retains the prompt until administrative tail classification");
            const auto held_state = early_tail.protocol_state;
            early_tail = inspect_readiness(purpose, held_state, "\"assistant\"}\n");
            require(early_tail.failure_reason == "unexpected_preprompt_output" && !early_tail.stdin_bytes,
                "a completed model tail still refuses input after a verified catalog");
            addon::Structured_inspection ended;
            ended.protocol_state = held_state;
            ended.final = true;
            ended.timed_out = true;
            require(claude::inspect_session(purpose, ended).failure_reason == "process_timed_out",
                "verified-but-unreleased timeout cannot release the prompt");
            ended.timed_out = false;
            const auto exited = claude::inspect_session(purpose, ended);
            require(exited.failure_reason == "mcp_readiness_incomplete" && !exited.stdin_bytes,
                "verified-but-unreleased process exit cannot release the prompt");
        }
        auto init = claude::object(R"({"type":"system","subtype":"init","session_id":"root","permissionMode":"plan","tools":[],"mcp_servers":[{"name":"logonomic-task-readiness-task-coordination","status":"connected"},{"name":"logonomic-task-readiness-task-delegation","status":"connected"}],"plugins":[],"cwd":""})");
        const auto state = claude::object(released);
        const auto* session_tools = state.find(purpose == addon::Structured_purpose::READ_ONLY_REVIEW
            ? "expected_tools" : "task_tools");
        require(session_tools != nullptr, "the prepared state names the tools the session must carry");
        init.set("tools", *session_tools);
        init.set("cwd", *state.find("working_directory"));
        result = inspect_readiness(purpose, released, addon::serialize_json(init) + '\n');
        require(!result.complete && !result.failure_reason && !result.stdin_bytes && !result.close_stdin,
            "actual native initialization follows readiness without a second input release");
        result = inspect_readiness(purpose, result.protocol_state,
            R"({"type":"result","session_id":"child","subtype":"success","is_error":false,"result":"Child output"})" "\n"
            R"({"type":"result","session_id":"root","subtype":"success","is_error":false,"result":"Delegated root output"})" "\n");
        require(!result.complete && !result.failure_reason && !result.result_bytes && !result.stdin_bytes,
            "both delegated purposes retain the authenticated root result until EOF settles");
        addon::Structured_inspection final;
        final.protocol_state = result.protocol_state;
        final.final = true;
        const auto completed = claude::inspect_session(purpose, final);
        require(completed.complete && completed.result_bytes == "Delegated root output" && !completed.failure_reason,
            "final successful exit authenticates the retained delegated root result");
        final.exit_code = 1;
        require(claude::inspect_session(purpose, final).failure_reason == "process_failed",
            "delegated root result cannot override failed process exit after EOF");
        if (purpose == addon::Structured_purpose::READ_ONLY_REVIEW) {
            init.find("tools")->push_back(Json("Write"));
            require(inspect_readiness(purpose, released, addon::serialize_json(init) + '\n').failure_reason ==
                "review_surface_mismatch", "readiness does not relax review's native surface attestation");
        }

        for (const auto* unexpected : {
                "{\"type\":\"assistant\"}\n", "{\"type\":\"result\"}\n",
                "{\"type\":\"system\",\"subtype\":\"init\",\"session_id\":\"root\"}\n"})
        {
            const auto refused = inspect_readiness(purpose, initial, unexpected);
            require(refused.failure_reason == "unexpected_preprompt_output" && !refused.stdin_bytes,
                "both purposes reject native model/init output before readiness");
        }
        auto wrong = claude::object(ack);
        wrong.find("response")->set("request_id", Json("unrelated"));
        require(inspect_readiness(purpose, initial, addon::serialize_json(wrong) + '\n').failure_reason ==
            "mcp_control_response_mismatch", "uncorrelated native acknowledgment is refused");
        wrong = claude::object(ack);
        wrong.find("response")->set("subtype", Json("error"));
        require(inspect_readiness(purpose, initial, addon::serialize_json(wrong) + '\n').failure_reason ==
            "mcp_control_unavailable", "unsupported native control operation refuses task release");
        require(inspect_readiness(purpose, initial, ack + ack).failure_reason == "unexpected_preprompt_output",
            "duplicate buffered acknowledgments cannot advance unsent requests");
        require(inspect_readiness(purpose, released, control_reply(status_state, catalog)).failure_reason ==
            "mcp_control_response_mismatch", "late status cannot release the task twice");
        for (const auto& phase_state : {initial, status_state}) {
            for (const int defect : {0, 1, 2, 3}) {
                auto reply = claude::object(control_reply(phase_state));
                auto* response = reply.find("response");
                if (defect == 0) response->erase("response");
                if (defect == 1) response->set("response", Json("not-an-object"));
                if (defect == 2) response->set("subtype", Json("error"));
                if (defect == 3) reply.set("type", Json("assistant"));
                const auto refused = inspect_readiness(purpose, phase_state, addon::serialize_json(reply) + '\n');
                require(refused.failure_reason ==
                    (defect == 3 ? "unexpected_preprompt_output" : "mcp_control_unavailable") && !refused.stdin_bytes,
                    "phase-specific native errors and malformed payloads refuse task release");
            }
        }
        const std::string hook = R"({"type":"system","subtype":"hook_started","session_id":"hook-root"})" "\n";
        const auto administrative = inspect_readiness(purpose, initial, hook);
        {
            require(!administrative.failure_reason && !administrative.stdin_bytes &&
                claude::text(claude::object(administrative.protocol_state), "root_session_id") == "hook-root",
                "both purposes retain exact startup hook provenance before readiness");
            auto partial_hook = inspect_readiness(purpose, initial, ack + hook.substr(0, 15));
            require(partial_hook.stdin_bytes && !partial_hook.failure_reason,
                "fragmented administrative output does not block an accepted control reply");
            partial_hook = inspect_readiness(purpose, partial_hook.protocol_state, hook.substr(15));
            require(!partial_hook.failure_reason && !partial_hook.stdin_bytes,
                "only the established startup hook can complete across the input boundary");
            const auto complete_hook = inspect_readiness(purpose, status_state,
                control_reply(status_state, catalog) + hook);
            for (const auto split : {std::size_t{1}, std::size_t{15}, hook.size() - 1}) {
                auto deferred = inspect_readiness(purpose, status_state,
                    control_reply(status_state, catalog) + hook.substr(0, split));
                require(!deferred.complete && !deferred.failure_reason && !deferred.stdin_bytes && !deferred.close_stdin,
                    "verified catalog defers task release for every split administrative tail");
                deferred = inspect_readiness(purpose, deferred.protocol_state, hook.substr(split));
                require(!deferred.failure_reason && deferred.stdin_bytes == complete_hook.stdin_bytes &&
                    deferred.close_stdin && deferred.protocol_state == complete_hook.protocol_state,
                    "administrative stream chunk boundaries do not change readiness disposition");
            }
        }
        auto partial_model = inspect_readiness(purpose, initial, ack + "{\"type\":");
        require(partial_model.stdin_bytes && !partial_model.close_stdin,
            "partial preprompt output cannot release task input");
        partial_model = inspect_readiness(purpose, partial_model.protocol_state, "\"assistant\"}\n");
        require(partial_model.failure_reason == "unexpected_preprompt_output" && !partial_model.stdin_bytes,
            "fragmentation cannot turn early model output into post-input output");
        for (const int defect : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13}) {
            auto invalid = catalog;
            auto& servers = invalid.find("mcpServers")->as_array();
            if (defect == 0) servers.erase(servers.begin() + 1);
            if (defect == 1) servers[0].set("status", Json("failed"));
            if (defect == 2) servers[0].set("tools", Json());
            if (defect == 3) servers[0].set("tools", claude::object(R"({"value":[]})"));
            if (defect == 4) servers[0].find("tools")->as_array().erase(servers[0].find("tools")->as_array().begin());
            if (defect == 5) servers.push_back(servers[0]);
            if (defect == 6) servers[0].find("tools")->push_back(servers[0].find("tools")->as_array()[0]);
            if (defect == 7) servers[0].erase("tools");
            if (defect == 8) servers[0].find("tools")->push_back(Json("not-a-tool"));
            if (defect == 9) servers[0].find("tools")->push_back(Json::object());
            if (defect == 10) servers.push_back(Json("not-a-server"));
            if (defect == 11) invalid.erase("mcpServers");
            if (defect == 12) servers[1].set("tools", *servers[0].find("tools"));
            if (defect == 13) invalid.set("mcpServers", Json::object());
            const auto refused = inspect_readiness(purpose, status_state, control_reply(status_state, invalid));
            require(refused.failure_reason == "required_mcp_unavailable" && !refused.stdin_bytes && !refused.close_stdin,
                "incomplete or ambiguous live catalog never releases task bytes");
        }
        addon::Structured_inspection ended;
        ended.protocol_state = initial;
        ended.final = true;
        ended.timed_out = true;
        const auto timeout = claude::inspect_session(purpose, ended);
        require(timeout.failure_reason == "process_timed_out" && !timeout.stdin_bytes,
            "readiness deadline closes without task release");
        ended.timed_out = false;
        require(claude::inspect_session(purpose, ended).failure_reason == "mcp_readiness_incomplete",
            "clean native exit without readiness cannot authenticate the task");
    }
}

// A delegated request from the direct launcher `claude` followed by `options`,
// with one Task server.
addon::Delegated_prepare_request posture_request(addon::Structured_purpose purpose, std::vector<std::string> options)
{
    addon::Delegated_prepare_request request;
    request.purpose = purpose;
    request.prompt  = "Review the retry budget.";
    request.context.launcher.runner_key = "direct";
    request.context.launcher_argv = {"claude"};
    request.context.launcher_argv.insert(request.context.launcher_argv.end(), options.begin(), options.end());
    for (const auto& word : request.context.launcher_argv) {
        request.context.launcher.script_body += (request.context.launcher.script_body.empty() ? "" : " ") + word;
    }
    request.context.working_directory = claude::path_string(fs::current_path());
    addon::Launch_environment environment;
#ifdef _WIN32
    environment.platform_id = "windows-x86_64";
#else
    environment.platform_id = addon::host_platform_id();
#endif
    environment.effective_environment = Json::object();
    environment.effective_environment.set("CLAUDE_CONFIG_DIR", Json(request.context.working_directory));
    request.context.launch_environment = environment;
    request.control.task_id = "posture-task";
    request.control.private_artifact_root = claude::path_string(fs::current_path() / "private-posture");
    request.control.connection = addon::Delegated_connection{
        claude::path_string(fs::current_path() / "host relay"),
        claude::path_string(fs::current_path() / "private-posture/.logonomic/task-servers.json")};
    addon::Delegated_task_server server;
    server.server_id = "delegation";
    server.program   = claude::path_string(fs::current_path() / "task-control");
    server.actions   = {"targets", "submit"};
    request.control.servers.push_back(std::move(server));
    return request;
}

Context posture_prepare(const addon::Delegated_prepare_request& request, bool delegated)
{
    Context invocation;
    invocation.m_request.operation = "prepare";
    if (delegated) {
        invocation.m_request.capability.capability_id = "delegated-session";
        invocation.m_request.payload = addon::encode_delegated_prepare_request(request, 3);
    }
    else {
        addon::Structured_prepare_request structured;
        structured.purpose = request.purpose;
        structured.context = request.context;
        structured.prompt  = request.prompt;
        invocation.m_request.capability.capability_id = "structured-session";
        invocation.m_request.payload = addon::encode_structured_prepare_request(structured, 3);
    }
    claude::handle_sessions(invocation, fs::current_path());
    return invocation;
}

// How Claude reads a prepared process's argv, whose executable is the
// launcher's own program.
Claude_reading prepared_reading(const Context& invocation, bool delegated)
{
    addon::Contract_failure failure;
    const auto process = delegated
        ? addon::decode_delegated_prepare_result(invocation.m_payload, &failure, 3)->process
        : addon::decode_structured_prepare_result(invocation.m_payload, &failure, 3)->process;
    std::vector<std::string> argv{"claude"};
    argv.insert(argv.end(), process.argv.begin(), process.argv.end());
    return claude_reading(argv);
}

bool has_option(const Claude_reading& reading, std::string_view name)
{
    return std::any_of(reading.options.begin(), reading.options.end(),
        [name](const auto& option) { return option.first == name; });
}

// Oracle: the owner's rule for delegated runs (2026-09-28): a run gets the
// permission posture its launcher specifies, its options and the user's own
// Claude settings, and the package pre-approves only its own Task tools.
// Claude Code 2.1.282's headless behaviour was recorded against a scripted
// endpoint: default mode denies an ungranted Write and runs a Task tool named
// by --allowedTools; the launcher's --permission-mode acceptEdits admits the
// Write; two --allowedTools occurrences both apply; a user deny rule for a
// Task server removes its tools from the session.
void permission_posture_contract()
{
    using Purpose = addon::Structured_purpose;
    const std::string task_tools =
        "mcp__logonomic-task-posture-task-delegation__targets,mcp__logonomic-task-posture-task-delegation__submit";
    for (const bool delegated : {true, false}) {
        const auto request = posture_request(Purpose::IMPLEMENTATION, {"--permission-mode", "acceptEdits"});
        const auto invocation = posture_prepare(request, delegated);
        require(invocation.m_status == addon::Result_status::SUCCESS, "implementation is prepared");
        const auto reading = prepared_reading(invocation, delegated);
        require(reading.values("--permission-mode") == std::vector<std::string>{"acceptEdits"} &&
            !has_option(reading, "--dangerously-skip-permissions"),
            "implementation runs in the launcher's own permission mode");
        require(delegated
            ? reading.values("--allowedTools") == std::vector<std::string>{task_tools}
            : !has_option(reading, "--allowedTools"),
            "implementation pre-approves exactly its own Task tools and nothing else");
    }

    const std::string task_tool = "mcp__logonomic-task-posture-task-delegation__submit";
    for (const auto purpose : {Purpose::IMPLEMENTATION, Purpose::READ_ONLY_REVIEW}) {
        auto state = protocol(purpose == Purpose::READ_ONLY_REVIEW);
        state.set("task_tools", claude::strings({task_tool}));
        state.set("expected_tools", claude::strings({"Read", "Glob", "Grep", task_tool}));
        auto init = claude::object(R"({"type":"system","subtype":"init","session_id":"root","permissionMode":"plan","tools":["Read","Glob","Grep"],"mcp_servers":[],"plugins":[],"cwd":""})");
        init.set("cwd", Json(claude::path_string(fs::current_path())));
        addon::Structured_inspection input;
        input.protocol_state = addon::serialize_json(state);
        input.stdout_bytes = addon::serialize_json(init) + '\n';
        require(claude::inspect_session(purpose, input).failure_reason == "task_tools_not_permitted",
            "a session whose settings removed a Task tool cannot run the task");
        init.find("tools")->push_back(Json(task_tool));
        input.stdout_bytes = addon::serialize_json(init) + '\n';
        const auto initialized = claude::inspect_session(purpose, input);
        require(!initialized.complete && !initialized.failure_reason, "a session with its Task tools runs");
        input.protocol_state = initialized.protocol_state;
        input.stdout_bytes = R"({"type":"result","session_id":"root","subtype":"success","is_error":false,"result":"Done","permission_denials":[{"tool_name":")" +
            task_tool + R"(","tool_use_id":"toolu_1","tool_input":{}}]})" "\n";
        require(claude::inspect_session(purpose, input).failure_reason == "task_tools_not_permitted",
            "a refused Task tool call fails the task");
        input.stdout_bytes = R"({"type":"result","session_id":"root","subtype":"success","is_error":false,"result":"Done","permission_denials":[{"tool_name":"Write","tool_use_id":"toolu_1","tool_input":{}}]})" "\n";
        require(!claude::inspect_session(purpose, input).failure_reason,
            "a denial of another tool is the launcher's posture, not a failure");
    }
}

// Oracle: the owner's read-only purpose and launcher-posture requirement,
// together with the recorded Claude Code 2.1.282 scripted review stream.
void review_contract()
{
    constexpr auto purpose = addon::Structured_purpose::READ_ONLY_REVIEW;
    int failures = 0;
    const auto check = [&failures](bool condition, const char* message) {
        if (!condition) {
            std::cerr << "Review: " << message << '\n';
            ++failures;
        }
    };
    for (const bool delegated : {true, false}) {
        auto request = posture_request(purpose,
            {"--disallowedTools", "Read(private/**)", "--settings", "review-settings.json", "--model", "opus"});
        // The host's normalized argv is the actual invocation, including a
        // valid embedded quotation in an option name.
        request.context.launcher.script_body =
            "claude --disallowed\"Tools\" Read(private/**) --settings review-settings.json --model opus";
        const auto invocation = posture_prepare(request, delegated);
        const auto reading = prepared_reading(invocation, delegated);
        check(reading.values("--disallowedTools") == std::vector<std::string>{"Read(private/**)"} &&
            reading.values("--settings") == std::vector<std::string>{"review-settings.json"} &&
            reading.values("--model") == std::vector<std::string>{"opus"},
            "normalized quoted launcher restrictions, settings and model survive review");
        check(!has_option(reading, "--safe-mode") && !has_option(reading, "--setting-sources"),
            "review keeps user hooks and all settings sources");
        check(!has_option(reading, "--permission-mode"), "review preserves the default permission mode");
        const std::string task_tools =
            "mcp__logonomic-task-posture-task-delegation__targets,mcp__logonomic-task-posture-task-delegation__submit";
        check(reading.values("--tools") == std::vector<std::string>{
            delegated ? "Read,Glob,Grep," + task_tools : "Read,Glob,Grep"},
            "review exposes only reads and its own Task tools");
        check(reading.values("--allowedTools") ==
            (delegated ? std::vector<std::string>{task_tools} : std::vector<std::string>{}),
            "review pre-approves only its own Task tools");
        for (const std::string option : {"--tools", "--mcp-config", "--"}) {
            request = posture_request(purpose, {option, "unsafe"});
            request.context.launcher.script_body = "claude \"" + option + "\" unsafe";
            bool refused = false;
            try { posture_prepare(request, delegated); }
            catch (const std::exception& error) { refused = std::string(error.what()) == "review_launcher_conflict"; }
            check(refused, "normalized quoted conflicting options are refused");
        }
        request = posture_request(purpose, {});
        request.context.launcher.script_body = "claude --disallowedTools Read";
        bool refused = false;
        try { posture_prepare(request, delegated); }
        catch (const std::exception& error) { refused = std::string(error.what()) == "unsupported_launcher"; }
        check(refused, "review refuses a launcher whose effective argv is not established");
    }

    auto init = claude::object(R"({"type":"system","subtype":"init","session_id":"root","permissionMode":"plan","tools":["Glob","Grep"],"mcp_servers":[],"plugins":[{"name":"agents-md","path":"builtin","source":"agents-md@builtin"}],"cwd":""})");
    init.set("cwd", Json(claude::path_string(fs::current_path())));
    addon::Structured_inspection input;
    input.protocol_state = addon::serialize_json(protocol(true));
    input.stdout_bytes = addon::serialize_json(init) + '\n';
    auto inspected = claude::inspect_session(purpose, input);
    check(!inspected.failure_reason, "builtin plugin and user-denied Read preserve the read-only ceiling");
    init.set("permissionMode", Json("bypassPermissions"));
    init.set("plugins", Json::array());
    init.set("tools", claude::strings({"Read", "Glob", "Grep"}));
    input.stdout_bytes = addon::serialize_json(init) + '\n';
    check(!claude::inspect_session(purpose, input).failure_reason,
        "the read-only tool ceiling also admits a launcher's bypass mode");
    init.find("tools")->push_back(Json("Write"));
    input.stdout_bytes = addon::serialize_json(init) + '\n';
    check(claude::inspect_session(purpose, input).failure_reason == "review_surface_mismatch",
        "plugin metadata cannot authorize a writing tool");

    const auto prepared = delegated_fixture(purpose);
    const auto initial = prepared.inspection.protocol_state;
    for (const std::string subtype : {"commands_changed", "informational", "hook_started", "hook_progress", "hook_response"}) {
        const std::string event = "{\"type\":\"system\",\"subtype\":\"" + subtype + "\",\"session_id\":\"root\"}\n";
        auto result = inspect_readiness(purpose, initial, event);
        check(!result.failure_reason && !result.stdin_bytes,
            "administrative events do not fail or release a review before readiness");
        auto verified = claude::object(initial);
        verified.set("mcp_phase", Json("verified"));
        result = inspect_readiness(purpose, addon::serialize_json(verified), event.substr(0, 15));
        const bool buffered = !result.failure_reason && !result.stdin_bytes;
        check(buffered, "fragmented review administration withholds input until classification");
        if (buffered) {
            result = inspect_readiness(purpose, result.protocol_state, event.substr(15));
            check(!result.failure_reason && result.stdin_bytes && result.close_stdin,
                "classified administrative tail releases exactly the ready task prompt");
        }
    }
    auto delegated_init = init;
    const auto delegated_state = claude::object(initial);
    delegated_init.set("tools", *delegated_state.find("expected_tools"));
    auto servers = Json::array();
    for (const auto& name : delegated_state.find("expected_mcp_servers")->as_array()) {
        auto server = Json::object();
        server.set("name", name);
        server.set("status", Json("connected"));
        server.set("source", Json("dynamic"));
        servers.push_back(std::move(server));
    }
    delegated_init.set("mcp_servers", servers);
    auto released = delegated_state;
    released.set("mcp_phase", Json("released"));
    check(!inspect_readiness(purpose, addon::serialize_json(released), addon::serialize_json(delegated_init) + '\n').failure_reason,
        "current native MCP server metadata preserves exact connected server attestation");

    std::ifstream fixture(fs::path(__FILE__).parent_path() / "fixtures/review_2_1_282.jsonl");
    require(bool(fixture), "recorded review fixture opens");
    std::string stream;
    for (std::string line; std::getline(fixture, line);) {
        if (line.find_first_not_of(" \r\t") == std::string::npos) continue;
        auto event = claude::object(line);
        if (claude::text(event, "subtype") == "init") event.set("cwd", *init.find("cwd"));
        stream += addon::serialize_json(event) + '\n';
    }
    for (const std::size_t chunk_size : {stream.size(), std::size_t{17}}) {
        input = {};
        input.protocol_state = addon::serialize_json(protocol(true));
        bool accepted = true;
        for (std::size_t offset = 0; offset < stream.size(); offset += chunk_size) {
            input.stdout_bytes = stream.substr(offset, chunk_size);
            inspected = claude::inspect_session(purpose, input);
            if (inspected.failure_reason || inspected.complete) { accepted = false; break; }
            input.protocol_state = inspected.protocol_state;
        }
        check(accepted, "recorded administrative review stream is accepted and waits for exit");
        if (!accepted) continue;
        input.stdout_bytes.clear();
        input.final = true;
        inspected = claude::inspect_session(purpose, input);
        check(inspected.complete && !inspected.failure_reason && inspected.result_bytes == "scripted-no-tool",
            "recorded review result authenticates after successful exit");
        input.exit_code = 1;
        check(claude::inspect_session(purpose, input).failure_reason == "process_failed",
            "recorded review cannot authenticate after failed exit");
    }
    input = {};
    input.protocol_state = addon::serialize_json(protocol(true));
    init.set("tools", claude::strings({"Read", "Glob", "Grep"}));
    init.set("plugins", Json::array());
    input.stdout_bytes = addon::serialize_json(init) + '\n' +
        R"({"type":"result","session_id":"root","subtype":"success","is_error":false,"result":"done"})" "\n";
    inspected = claude::inspect_session(purpose, input);
    check(!inspected.complete && !inspected.failure_reason, "standalone review waits for final exit");
    require(failures == 0, "read-only review contract failures");
}

void probe_contract()
{
    addon::Structured_inspection input;
    input.final = true;
    input.stdout_bytes = R"({"loggedIn":true})";
    require(claude::inspect_auth(input) == addon::Auth_status::AUTHENTICATED, "auth requires matching successful exit");
    input.exit_code = 1;
    require(claude::inspect_auth(input) == addon::Auth_status::UNKNOWN, "contradictory auth evidence stays unknown");
    input.stdout_bytes = R"({"loggedIn":false})";
    require(claude::inspect_auth(input) == addon::Auth_status::NOT_LOGGED_IN, "native logged-out exit semantics");
    input.exit_code = 0;
    input.stdout_bytes = R"({"type":"result","subtype":"success","is_error":false,"result":"Current session: 35% used, resets 3pm (Europe/Berlin)\nCurrent week: 21.5% used, resets September 17."})";
    const auto quota = claude::inspect_quota(input);
    require(quota.state == addon::Quota_status::READY && quota.primary->used_percent == 35 &&
        quota.secondary->used_percent == 21.5, "both native quota windows parsed");
    require(quota.secondary->reset_text == "September 17", "text reset retained without guessing epoch");
    // Native y() omits the reset suffix for a null API resets_at value.
    input.stdout_bytes = R"json({"type":"result","subtype":"success","local_command":"usage","result":"Current session: 0% used\nCurrent week (all models): 100% used · resets Oct 4, 10pm (Europe/Berlin)"})json";
    const auto unknown_reset = claude::inspect_quota(input);
    require(unknown_reset.state == addon::Quota_status::READY && unknown_reset.primary->used_percent == 0 &&
        !unknown_reset.primary->reset_text && unknown_reset.secondary->reset_text == "Oct 4, 10pm (Europe/Berlin)",
        "an unknown primary reset never borrows the weekly reset");
    input.stdout_bytes = R"({"type":"result","subtype":"success","is_error":false,"result":"Current session: 135.5% used, resets 3pm\nCurrent week: 121% used, resets September 17."})";
    const auto overage = claude::inspect_quota(input);
    require(overage.state == addon::Quota_status::READY && overage.primary->used_percent == 135.5 &&
        overage.secondary->used_percent == 121, "native quota overage remains observable without clamping");

    input.stdout_bytes =
        R"([{"type":"system","subtype":"init"},)"
        R"({"type":"result","subtype":"success","is_error":false,)"
        R"("local_command":"usage","result":"Total cost: $0.0000\n)"
        R"(Total duration (API): 0s\nUsage: 0 input, 0 output, 0 cache read, 0 cache write"}])";
    const auto print_mode = claude::inspect_quota(input);
    require(print_mode.state == addon::Quota_status::UNAVAILABLE && !print_mode.primary && !print_mode.secondary,
        "print-mode usage totals do not authenticate subscription quota windows");

    input.stdout_bytes =
        R"({"type":"result","subtype":"success","is_error":false,)"
        R"("local_command":"usage","result":"Current session: 35% used, resets 3pm"})";
    const auto incomplete = claude::inspect_quota(input);
    require(incomplete.state == addon::Quota_status::FAILED && !incomplete.primary && !incomplete.secondary,
        "incomplete quota evidence remains a failed observation without partial windows");
}

#ifdef _WIN32
void probe_profile_contract()
{
    // Windows environment names are case-insensitive, and the host passes the
    // launcher environment in its own spelling. Claude Code honours
    // Claude_Config_Dir, so the quota probe and the account must select it too.
    const auto root = fs::temp_directory_path() / ("claude-profile-" + addon::random_nonce_hex());
    const auto configured = root / "work-profile";
    const auto home = root / "home";
    fs::create_directories(root / "tools");
    fs::create_directories(configured);
    fs::create_directories(home / ".claude");
    std::ofstream(root / "tools/claude.exe").close();
    try {
        addon::Probe_prepare_request request;
        request.context.working_directory = claude::path_string(root);
        request.context.launcher.script_body = "claude";
        request.context.launcher.runner_key = "bash";
        request.context.launcher_argv = {"bash", "-c", "claude"};
        request.context.launch_environment = addon::Launch_environment{};
        request.context.launch_environment->platform_id = "windows-x86_64";
        auto& environment = request.context.launch_environment->effective_environment;
        environment.set("Path", Json(claude::path_string(root / "tools")));
        environment.set("UserProfile", Json(claude::path_string(home)));
        environment.set("Claude_Config_Dir", Json(claude::path_string(configured)));
        addon::Contract_failure failure;
        const auto probe_profile = [&]() {
            Context invocation;
            invocation.m_request.capability.capability_id = "quota";
            invocation.m_request.operation = "prepare";
            invocation.m_request.payload = addon::encode_probe_prepare_request(request, 2);
            claude::handle_probes(invocation, root);
            const auto prepared = addon::decode_structured_prepare_result(invocation.m_payload, &failure, 2);
            require(prepared.has_value(), "quota preparation yields a process proposal");
            for (const auto& [name, value] : prepared->process.environment) {
                if (name == "CLAUDE_CONFIG_DIR") return claude::account_key(addon::path_from_utf8(value));
            }
            throw std::runtime_error("quota probe names no Claude profile");
        };
        const auto resolved_account = [&]() {
            addon::Accounts_launcher_request accounts;
            accounts.launcher = request.context.launcher;
            accounts.launch_environment = *request.context.launch_environment;
            accounts.normalization_directory = claude::path_string(root);
            Context invocation;
            invocation.m_request.capability.capability_id = "accounts";
            invocation.m_request.operation = "resolve";
            invocation.m_request.payload = addon::encode_accounts_launcher_request(accounts);
            claude::handle_accounts(invocation);
            const auto resolution = addon::decode_account_resolution_result(invocation.m_payload, &failure);
            require(resolution && resolution->adapter_account_key, "account resolution names an account");
            return *resolution->adapter_account_key;
        };
        require(probe_profile() == claude::account_key(configured),
            "the quota probe runs against the profile named by Claude_Config_Dir");
        require(resolved_account() == claude::account_key(configured),
            "the account is the profile named by Claude_Config_Dir");
        environment.erase("Claude_Config_Dir");
        require(probe_profile() == claude::account_key(home / ".claude"),
            "without a profile override the quota probe uses UserProfile's .claude");
        require(resolved_account() == claude::account_key(home / ".claude"),
            "without a profile override the account is UserProfile's .claude");
    }
    catch (...) {
        fs::remove_all(root);
        throw;
    }
    fs::remove_all(root);
}
#endif

void usage_contract()
{
    const auto root = fs::temp_directory_path() / ("claude-usage-" + addon::random_nonce_hex());
    fs::create_directories(root / "projects/project");
    try {
        std::ofstream file(root / "projects/project/session.jsonl", std::ios::binary);
        file << R"({"type":"assistant","sessionId":"SessionCase","timestamp":"2026-09-12T12:00:00","message":{"id":"message-A","stop_reason":"end_turn","usage":{"input_tokens":12,"output_tokens":8,"cache_read_input_tokens":3,"cache_creation_input_tokens":2}}})" << '\n';
        file << R"({"type":"assistant","sessionId":"SessionCase","timestamp":"2026-09-12T12:00:00","message":{"id":"message-A","stop_reason":"end_turn","usage":{"input_tokens":12,"output_tokens":8,"cache_read_input_tokens":3,"cache_creation_input_tokens":2}}})" << '\n';
        file << R"({"type":"assistant","sessionId":"SessionCase","timestamp":"2026-09-12T12:00:00","message":{"id":"message-B","stop_reason":"end_turn","usage":{"input_tokens":7,"output_tokens":5}}})" << '\n';
        file << "{unfinished";
        file.close();
        Context context;
        const auto sessions = claude::scan_usage(root, context);
        require(sessions.size() == 1 && sessions[0].record.record_id == "SessionCase" && sessions[0].record.total_tokens == 37,
            "session aggregation deduplicates message id and excludes partial tail");
        require(sessions[0].message_count == 2 && sessions[0].category_coverage[2] == 1 && sessions[0].category_values[2] == 3,
            "partial cache coverage retained separately from observed zero");
        require(sessions[0].record.cache_read.applicable && sessions[0].record.cache_read.value == 3 &&
            sessions[0].record.cache_read.coverage &&
            sessions[0].record.cache_read.coverage->support == addon::Usage_support::UNKNOWN &&
            sessions[0].record.cache_read.coverage->covered_records == 1 &&
            sessions[0].record.cache_read.coverage->total_records == 2,
            "partial category remains known with incomplete coverage");
        addon::Contract_failure failure;
        require(addon::decode_usage_record(addon::encode_usage_record(sessions[0].record, 2), &failure, 2).has_value(),
            "native aggregate satisfies rich usage contract");
        auto native = claude::object(R"({"path":"placeholder","size":0,"modified_msecs":0,"records":[{"record_id":"retained-message","session_id":"SessionCase","completed_date":"2026-09-11","total_tokens":30,"source_path":"placeholder","recorded_cwd":"","model":"sonnet","capabilities":{"token_breakdown":{"version":1,"input_tokens":30}}}]})");
        const auto missing = claude::path_string(root / "projects/project/deleted.jsonl");
        native.set("path", addon::Json_value(missing));
        native.find("records")->as_array()[0].set("source_path", addon::Json_value(missing));
        const auto seed_bytes = addon::serialize_json(native) + "\n";
        const auto seed_path = root / "native-seed.jsonl";
        std::ofstream seed(seed_path, std::ios::binary);
        seed << seed_bytes;
        seed.close();
        claude::Usage_cache_input cache;
        cache.native_seed = seed_path;
        cache.seed_sha256_hex = addon::to_hex(addon::Sha256::digest(seed_bytes));
        cache.storage_file = root / "owned-cache.jsonl";
        const auto imported = claude::scan_usage(root, context, cache);
        require(imported.size() == 1 && imported[0].record.total_tokens == 67,
            "native missing-file messages participate before same-session replacement");
        fs::remove_all(root / "projects");
        cache.native_seed.reset();
        cache.cache_required = true;
        const auto retained = claude::scan_usage(root, context, cache);
        require(retained.size() == 1 && retained[0].record.total_tokens == 67,
            "owned cache preserves imported and deleted projects-subtree evidence");
        const auto new_profile = root / "new-profile";
        fs::create_directory(new_profile);
        require(claude::scan_usage(new_profile, context).empty(),
            "new valid profile without projects subtree is empty");
        {
            std::ofstream projects_file(root / "projects", std::ios::binary);
            projects_file << "not a directory";
        }
        bool projects_path_refused = false;
        try { (void)claude::scan_usage(root, context, cache); }
        catch (const std::exception&) { projects_path_refused = true; }
        require(projects_path_refused,
            "existing non-directory projects path rejects retained cache evidence");
        fs::remove(root / "projects");
        fs::remove(*cache.storage_file);
        bool refused = false;
        try { (void)claude::scan_usage(root, context, cache); }
        catch (const std::exception&) { refused = true; }
        require(refused, "required historical cache loss blocks replacement");
    }
    catch (...) {
        fs::remove_all(root);
        throw;
    }
    fs::remove_all(root);
}

void empty_usage_cache_contract()
{
    const auto root = fs::temp_directory_path() / ("claude-empty-cache-" + addon::random_nonce_hex());
    fs::create_directory(root);
    try {
        Context context;
        claude::Usage_cache_input cache;
        cache.native_seed = root / "native-seed.jsonl";
        cache.seed_sha256_hex = addon::to_hex(addon::Sha256::digest(""));
        cache.storage_file = root / "owned-cache.jsonl";
        cache.cache_required = true;
        std::ofstream seed(*cache.native_seed, std::ios::binary);
        seed.close();
        require(bool(seed), "empty native seed fixture written");
        const auto require_missing_evidence = [&] {
            bool refused = false;
            try { (void)claude::scan_usage(root, context, cache); }
            catch (const std::runtime_error& error) {
                refused = std::string_view(error.what()) == "usage_required_cache_unavailable";
            }
            require(refused, "required history needs actual retained message evidence");
        };
        require_missing_evidence();
        require(!fs::exists(*cache.storage_file), "refused empty seed must not publish an owned cache");

        // Fresh empty history remains legal when the host requires no retained evidence.
        cache.cache_required = false;
        require(claude::scan_usage(root, context, cache).empty(),
            "optional empty native seed supports fresh empty history");
        require(fs::is_regular_file(*cache.storage_file), "optional scan persisted its empty cache");
        std::ifstream before(*cache.storage_file, std::ios::binary);
        require(bool(before), "existing cache opens before refusal");
        const std::string accepted_before((std::istreambuf_iterator<char>(before)), std::istreambuf_iterator<char>());
        before.close();
        fs::remove(*cache.native_seed);
        cache.native_seed.reset();
        cache.cache_required = true;
        require_missing_evidence();
        std::ifstream after(*cache.storage_file, std::ios::binary);
        require(bool(after), "existing cache opens after refusal");
        const std::string accepted_after((std::istreambuf_iterator<char>(after)), std::istreambuf_iterator<char>());
        after.close();
        require(accepted_before == accepted_after, "refusal preserves the existing cache bytes");
        cache.cache_required = false;
        require(claude::scan_usage(root, context, cache).empty(),
            "optional header-only cache remains valid on reload");
        fs::remove(*cache.storage_file);
        cache.cache_required = true;
        require_missing_evidence();

        // A nonempty wrapper with no records is still not historical evidence.
        cache.native_seed = root / "native-seed.jsonl";
        auto wrapper = addon::Json_value::object();
        wrapper.set("path", addon::Json_value(claude::path_string(root / "missing.jsonl")));
        wrapper.set("records", addon::Json_value::array());
        const auto wrapped = addon::serialize_json(wrapper) + "\n";
        cache.seed_sha256_hex = addon::to_hex(addon::Sha256::digest(wrapped));
        std::ofstream wrapper_file(*cache.native_seed, std::ios::binary);
        wrapper_file << wrapped;
        wrapper_file.close();
        require(bool(wrapper_file), "zero-record native wrapper fixture written");
        require_missing_evidence();
        require(!fs::exists(*cache.storage_file), "zero-record refusal must not publish a cache");
    }
    catch (...) {
        fs::remove_all(root);
        throw;
    }
    fs::remove_all(root);
}

void retained_message_contract()
{
    for (const bool native_seed : {false, true}) {
        const auto root = fs::temp_directory_path() / ("claude-retained-" + addon::random_nonce_hex());
        const auto source = root / "projects/project/session.jsonl";
        fs::create_directories(source.parent_path());
        fs::create_directory(root / ".git");
        try {
            const auto message = [
                    &root
                ](
                    const char* id,
                    std::uint64_t tokens)
                {
                    auto row = claude::object(R"({"type":"assistant","sessionId":"SessionCase","timestamp":"2026-09-12T12:00:00","message":{"stop_reason":"end_turn","usage":{}}})");
                    row.set("cwd", Json(claude::path_string(root)));
                    row.find("message")->set("id", Json(id));
                    row.find("message")->find("usage")->set("input_tokens", Json(tokens));
                    return row;
                };
            const auto write_rows = [
                    &source
                ](
                    const std::vector<Json>& rows,
                    std::string_view tail)
                {
                    std::ofstream stream(source, std::ios::binary | std::ios::trunc);
                    for (const auto& row : rows) {
                        stream << addon::serialize_json(row) << '\n';
                    }
                    stream << tail;
                    stream.close();
                    require(bool(stream), "transcript fixture written");
                };
            Context context;
            claude::Usage_cache_input cache;
            cache.storage_file = root / "owned-cache.jsonl";
            const auto first  = message("message-A", 25);
            const auto second = message("message-B", 12);
            if (native_seed) {
                Json records = Json::array();
                for (const auto& row : {first, second}) {
                    auto record = claude::object(R"({"session_id":"SessionCase","completed_date":"2026-09-12"})");
                    record.set("record_id", *row.find("message")->find("id"));
                    record.set("total_tokens", *row.find("message")->find("usage")->find("input_tokens"));
                    record.set("source_path", Json(claude::path_string(source)));
                    record.set("recorded_cwd", Json(claude::path_string(root)));
                    records.push_back(std::move(record));
                }
                Json wrapper = Json::object();
                wrapper.set("path", Json(claude::path_string(source)));
                wrapper.set("records", std::move(records));
                const auto bytes = addon::serialize_json(wrapper) + "\n";
                cache.native_seed = root / "native-seed.jsonl";
                cache.seed_sha256_hex = addon::to_hex(addon::Sha256::digest(bytes));
                cache.cache_required = true;
                std::ofstream seed(*cache.native_seed, std::ios::binary);
                seed << bytes;
                seed.close();
                require(bool(seed), "native seed fixture written");
            }
            else {
                write_rows({first, second}, {});
                require(claude::scan_usage(root, context, cache)[0].record.total_tokens == 37,
                    "completed messages establish package retained evidence");
            }

            // usage-scan@2 requires retained message identities even when the
            // surviving source ends before the previously completed message.
            write_rows({first}, "{unfinished");
            auto sessions = claude::scan_usage(root, context, cache);
            require(sessions.size() == 1 && sessions[0].record.total_tokens == 37,
                "partial surviving source preserves both retained completed messages");
            addon::Contract_failure failure;
            const auto decoded = addon::decode_usage_record(
                addon::encode_usage_record(sessions[0].record, 2), &failure, 2);
            require(decoded && decoded->repository_usage && decoded->repository_usage->allocations.size() == 1 &&
                decoded->repository_usage->allocations[0].total_tokens == 37,
                "single real repository allocation satisfies usage-scan@2 codec");
            if (cache.native_seed) {
                fs::remove(*cache.native_seed);
                cache.native_seed.reset();
            }
            cache.cache_required = true;
            auto unfinished = message("message-B", 2);
            unfinished.find("message")->set("stop_reason", Json());
            write_rows({first, unfinished}, {});
            require(claude::scan_usage(root, context, cache)[0].record.total_tokens == 37,
                "in-progress lower message cannot replace completed evidence after cache reload");
            write_rows({message("message-A", 5)}, {});
            require(claude::scan_usage(root, context, cache)[0].record.total_tokens == 17,
                "completed same-ID correction replaces its own value and retains absent message");
            write_rows({message("message-B", 3), message("message-C", 4)}, {});
            require(claude::scan_usage(root, context, cache)[0].record.total_tokens == 12,
                "completed corrections and new message IDs reconcile without a monotonic total");
            write_rows({message("message-B", 3), message("message-B", 6)}, {});
            bool conflicting_usage = false;
            try {
                (void)claude::scan_usage(root, context, cache);
            }
            catch (const std::runtime_error& error) {
                conflicting_usage = std::string_view(error.what()) == "conflicting_message_usage";
            }
            require(conflicting_usage, "fresh duplicate message IDs retain conflicting usage rejection");
            fs::remove(source);
            require(claude::scan_usage(root, context, cache)[0].record.total_tokens == 12,
                "corrected message evidence survives source deletion and cache reload");
        }
        catch (...) {
            fs::remove_all(root);
            throw;
        }
        fs::remove_all(root);
    }
}

void quota_reset_label_contract()
{
    using namespace logonomic::cli_addon;
    const std::vector<std::pair<std::string,std::string>> cases{
        {"7pm (Europe/Berlin)", "7pm (Europe/Berlin)"},
        {"7pm (Europe/Berlin))., ", "7pm (Europe/Berlin)"},
        {"7pm (Europe/Berlin).)", "7pm (Europe/Berlin)"},
        {"Sep 18 (Europe/Berlin)\t", "Sep 18 (Europe/Berlin)"},
        {"7pm., \t", "7pm"},
        {"7pm)", "7pm"},
        {"7pm.)", "7pm"},
        {"7pm.) ,)\t", "7pm"},
        {" ", ""},
    };
    for (const auto& [input, expected] : cases) {
        Json_value event=Json_value::object();
        event.set("type", Json_value("result"));
        event.set("is_error", Json_value(false));
        event.set("result", Json_value("Current session: 12% used, resets " + input +
            "\nCurrent week: 34% used, resets " + input));
        Structured_inspection inspection;
        inspection.final=true; inspection.exit_code=0;
        inspection.stdout_bytes=serialize_json(event);
        const auto observation=claude::inspect_quota(inspection);
        const bool good=expected.empty() ? (!observation.primary && !observation.secondary) :
            (observation.primary && observation.secondary &&
             observation.primary->reset_text.value_or("")==expected &&
             observation.secondary->reset_text.value_or("")==expected);
        std::cout << (good?"PASS":"FAIL") << " reset_label=[" << input << "] expected=[" << expected << "]\n";
        require(good, "ARC-005 quota reset label must preserve balanced parentheses in both windows");
    }
}

void account_name_contract()
{
    // Oracle: accounts@1 names an account with a filesystem profile after the
    // profile's folder through the SDK's account_display_name.
#ifdef _WIN32
    const std::string platform = "windows-x86_64";
    const std::string home     = "C:/Users/Jane Doe";
    const std::string home_env = "USERPROFILE";
#else
    const std::string platform = addon::host_platform_id();
    const std::string home     = "/home/jane";
    const std::string home_env = "HOME";
#endif
    const auto discovered_account = [&](const std::string& config_dir) -> std::optional<addon::Account_record> {
        addon::Accounts_launcher_request request;
        request.launcher.script_body = "claude";
        request.launcher.runner_key  = addon::k_runner_direct;
        request.launch_environment.platform_id = platform;
        request.launch_environment.effective_environment = Json::object();
        request.launch_environment.effective_environment.set(home_env, Json(home));
        request.launch_environment.effective_environment.set("CLAUDE_CONFIG_DIR", Json(config_dir));
        request.default_home_fallback   = home;
        request.normalization_directory = home;
        Context discovery;
        discovery.m_request.capability.capability_id = "accounts";
        discovery.m_request.operation = addon::k_accounts_discover_operation;
        discovery.m_request.payload   = addon::encode_accounts_launcher_request(request);
        claude::handle_accounts(discovery);
        addon::Contract_failure failure;
        const auto accounts = addon::decode_accounts_result(discovery.m_payload, &failure);
        return accounts && accounts->size() == 1 ? std::optional(accounts->front()) : std::nullopt;
    };
    const auto discovered_name = [&](const std::string& config_dir) {
        const auto account = discovered_account(config_dir);
        return account ? account->display_name : std::string{};
    };
    require(discovered_name(home + "/.claude-work/") == ".claude-work",
        "a trailing separator does not hide the profile folder's name");
    const std::string long_home = home + "/.claude-account-for-the-varinomics-nightly-release-builds";
    require(discovered_name(long_home) == addon::account_display_name(long_home),
        "an over-long folder name is fitted by the SDK's account display-name rule");
#ifdef _WIN32
    // A profile on a share that cannot be reached is an unavailable account;
    // it must not fail the discovery of every account.
    const auto unreachable = discovered_account("\\\\localhost\\logonomic-absent-share\\");
    require(unreachable && unreachable->display_name == "\\\\localhost\\logonomic-absent-share" &&
        unreachable->status == addon::Account_status::UNAVAILABLE,
        "a profile on an unreachable share is discovered as an unavailable account");
#endif
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "usage: claude_contract_tests <package directory>\n";
        return 2;
    }
    try {
        review_contract();
        launcher_contract();
        launcher_discovery_contract();
        launcher_discovery_candidate_limit();
        launcher_discovery_scan_limit();
        launcher_directory_contract();
        initial_prompt_contract();
        wake_preparation_contract();
        wake_binding_contract();
        lifecycle_contract();
        declared_profile_admits_every_shape(addon::path_from_utf8(argv[1]));
        structured_contract();
        delegated_readiness_contract();
        permission_posture_contract();
        probe_contract();
#ifdef _WIN32
        probe_profile_contract();
#endif
        quota_reset_label_contract();
        account_name_contract();
        usage_contract();
        empty_usage_cache_contract();
        retained_message_contract();
        std::cout << "Claude native contract checks passed.\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
