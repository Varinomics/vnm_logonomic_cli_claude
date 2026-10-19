#include "claude_launcher.h"
#include "claude_support.h"
#include "claude_accounts.h"

#include <logonomic_cli_addon/launcher_utilities.h>
#include <logonomic_cli_addon/json.h>
#include <logonomic_cli_addon/limits.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/presentation_record.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <system_error>

namespace claude {
namespace addon = logonomic::cli_addon;
using addon::Launcher_whitespace;
namespace {
using Environment = std::vector<std::pair<std::string, std::string>>;
constexpr std::size_t k_max_profile_directory_entries_to_scan = 256;

// Claude's options that take the next word as their value, whatever that word
// is, among the options an interactive launch may carry.
constexpr std::array<std::string_view, 15> k_value_options{
    "--agent", "--agents", "--append-system-prompt", "--autocompact", "--debug-file",
    "--effort", "--model", "-n", "--name", "--permission-mode", "--plugin-dir",
    "--remote-control-session-name-prefix", "--setting-sources", "--settings", "--system-prompt",
};
// An option whose value is optional: it takes the next word only when that
// word does not start with '-'.
constexpr std::string_view k_prompt_suggestions = "--prompt-suggestions";

std::string bounded_display_name(std::string_view value)
{
    if (auto fitted = logonomic::cli_addon::fitted_display_name(value)) {
        return *fitted;
    }
    return "Claude";
}

bool contains_environment_line_break(std::string_view value)
{
    return value.find_first_of("\r\n") != std::string_view::npos;
}

bool package_flag(std::string_view value)
{
    return value == "-y" || value == "--yes" || value == "--quiet" ||
        value == "--silent" || value == "--no-install" || value == "--bun";
}

std::optional<Launcher_command> recognize(std::string_view script, int depth)
{
    if (depth == 0) return std::nullopt;
    Environment inherited;
    std::istringstream lines{std::string(script)};
    for (std::string line; std::getline(lines, line);) {
        line = addon::trim(line, Launcher_whitespace::ASCII);
        if (line.empty() || line.starts_with('#') || line.starts_with("//") ||
            addon::lower(line).starts_with("rem ")) continue;
        for (std::string segment : addon::launcher_segments(line, Launcher_whitespace::ASCII)) {
            const std::string normalized = addon::lower(segment);
            std::optional<std::pair<std::string, std::string>> standalone;
            if (normalized.starts_with("set ")) standalone = addon::environment_assignment(
                segment.substr(4), Launcher_whitespace::ASCII);
            else
            if (normalized.starts_with("export ")) standalone = addon::environment_assignment(
                segment.substr(7), Launcher_whitespace::ASCII);
            else
            if (normalized.starts_with("$env:")) standalone = addon::environment_assignment(
                segment.substr(5), Launcher_whitespace::ASCII);
            if (standalone) {
                inherited.push_back(*standalone);
                continue;
            }
            Environment environment = inherited;
            // Leading assignment values may contain quoted whitespace, unlike
            // the command-token grammar used for the remainder of the line.
            static const std::regex leading(R"re(^([A-Za-z_][A-Za-z0-9_]*)=("[^"]*"|'[^']*'|[^\s]*)\s*)re");
            std::smatch match;
            while (std::regex_search(segment, match, leading)) {
                environment.emplace_back(match[1].str(), addon::unquote(match[2].str(), Launcher_whitespace::ASCII));
                segment.erase(0, match.length());
            }
            const auto words = addon::launcher_tokens(segment);
            std::size_t index = 0;
            while (index < words.size()) {
                const auto value = addon::environment_assignment(words[index], Launcher_whitespace::ASCII);
                const auto prefix = addon::lower(addon::unquote(words[index], Launcher_whitespace::ASCII));
                if (value) environment.push_back(*value);
                else
            if (prefix != "call" && prefix != "command" && prefix != "exec" && prefix != "&") break;
                ++index;
            }
            if (index == words.size()) continue;
            const std::string program = addon::unquote(words[index], Launcher_whitespace::ASCII);
            const std::string base = addon::base_name(program, Launcher_whitespace::ASCII);
            if (base == "claude" || base == "claude-code" || base.starts_with("claude-code@")) return Launcher_command{program, {}, environment};
            if (base == "npx" || base == "bunx") {
                std::vector<std::string> arguments;
                for (++index; index < words.size(); ++index) {
                    const auto argument = addon::unquote(words[index], Launcher_whitespace::ASCII);
                    arguments.push_back(argument);
                    if (argument.starts_with('-')) {
                        if (!package_flag(argument)) break;
                    }
                    else {
                        const auto package = addon::lower(argument);
                        if (package == "@anthropic-ai/claude-code" || package.starts_with("@anthropic-ai/claude-code@")) {
                            return Launcher_command{program, arguments, environment};
                        }
                        break;
                    }
                }
            }
            else
            if (base == "cmd" || base == "powershell" || base == "pwsh" ||
                     base == "bash" || base == "sh")
            {
                for (++index; index + 1 < words.size(); ++index) {
                    const auto argument = addon::lower(addon::unquote(words[index], Launcher_whitespace::ASCII));
                    if (argument != "/c" && argument != "-c" && argument != "-command") continue;
                    std::string tail;
                    for (++index; index < words.size(); ++index) {
                        if (!tail.empty()) tail += ' ';
                        tail += words[index];
                    }
                    auto command = recognize(addon::unquote(tail, Launcher_whitespace::ASCII), depth - 1);
                    if (command) {
                        command->environment.insert(command->environment.begin(), environment.begin(), environment.end());
                        return command;
                    }
                    break;
                }
            }
        }
    }
    return std::nullopt;
}


} // namespace

bool is_valid_claude_config_environment_text(std::string_view environment_text)
{
    return environment_text.size() <= logonomic::cli_addon::limits::k_max_environment_text_bytes &&
        logonomic::cli_addon::is_valid_utf8(environment_text) &&
        environment_text.find('\0') == std::string_view::npos &&
        !contains_environment_line_break(environment_text);
}

std::optional<Launcher_command> recognize_launcher(std::string_view script)
{
    return recognize(script, 4);
}

std::optional<std::size_t> cli_argument_index(const std::vector<std::string>& argv)
{
    if (argv.empty()) {
        return std::nullopt;
    }
    const auto base = addon::base_name(argv.front(), Launcher_whitespace::ASCII);
    if (base == "claude" || base == "claude-code" || base.starts_with("claude-code@")) {
        return 0;
    }
    if (base != "npx" && base != "bunx") {
        return std::nullopt;
    }
    for (std::size_t index = 1; index < argv.size(); ++index) {
        const auto& argument = argv[index];
        if (argument.starts_with('-')) {
            if (!package_flag(argument)) {
                return std::nullopt;
            }
            continue;
        }
        constexpr std::string_view package = "@anthropic-ai/claude-code";
        return argument == package ||
            (argument.starts_with(std::string(package) + '@') && argument.size() > package.size() + 1)
            ? std::optional(index) : std::nullopt;
    }
    return std::nullopt;
}

std::size_t claude_option_value_words(const std::vector<std::string>& argv, std::size_t index)
{
    if (index + 1 >= argv.size()) {
        return 0;
    }
    const auto& argument = argv[index];
    if (std::find(k_value_options.begin(), k_value_options.end(), argument) != k_value_options.end()) {
        return 1;
    }
    return argument == k_prompt_suggestions && !argv[index + 1].starts_with('-') ? 1 : 0;
}

bool interactive_resume_arguments(const std::vector<std::string>& argv)
{
    const auto cli_index = cli_argument_index(argv);
    if (!cli_index) {
        return false;
    }
    const std::vector<std::string_view> switches{
        "--allow-dangerously-skip-permissions", "--ax-screen-reader", "--brief", "--chrome",
        "--dangerously-skip-permissions", "--disable-slash-commands",
        "--exclude-dynamic-system-prompt-sections", "--ide", "--no-chrome", "--restricted",
        "--strict-mcp-config", "--verbose",
    };
    // Claude refuses to start an interactive session with prompt suggestions
    // on, which --prompt-suggestions alone and every value but Claude's own
    // off words turn on.
    const auto suggestions_off = [](std::string_view value) {
        const auto word = addon::lower(addon::trim(value, Launcher_whitespace::ASCII));
        return word == "0" || word == "false" || word == "no" || word == "off";
    };
    for (std::size_t index = *cli_index + 1; index < argv.size(); ++index) {
        const auto& argument = argv[index];
        if (argument == k_prompt_suggestions) {
            if (claude_option_value_words(argv, index) == 0 || !suggestions_off(argv[++index])) {
                return false;
            }
            continue;
        }
        if (argument.starts_with(std::string(k_prompt_suggestions) + '=')) {
            if (!suggestions_off(std::string_view(argument).substr(k_prompt_suggestions.size() + 1))) {
                return false;
            }
            continue;
        }
        if (claude_option_value_words(argv, index) == 1) {
            if (argv[++index].empty()) {
                return false;
            }
            continue;
        }
        const auto equals = argument.find('=');
        if (equals != std::string::npos && equals + 1 < argument.size() &&
            (std::find(k_value_options.begin(), k_value_options.end(), argument.substr(0, equals)) !=
                 k_value_options.end() ||
             argument.substr(0, equals) == "--debug"))
        {
            continue;
        }
        if (std::find(switches.begin(), switches.end(), argument) == switches.end()) {
            return false;
        }
    }
    return true;
}

bool direct_implementation_arguments(const logonomic::cli_addon::Session_launch_context& context)
{
    const auto literal = addon::literal_argv(context.launcher.script_body);
    if (context.launcher.runner_key != "direct" || literal != context.launcher_argv ||
        !cli_argument_index(literal))
    {
        return false;
    }
    const auto base = addon::base_name(literal.front(), Launcher_whitespace::ASCII);
    const auto program = addon::lower(literal.front());
    return (base == "npx" || base == "bunx") ||
        !(program.ends_with(".cmd") || program.ends_with(".bat") ||
          program.ends_with(".ps1") || program.ends_with(".sh"));
}

logonomic::cli_addon::Process_proposal cli_process(
    const logonomic::cli_addon::Session_launch_context& context, bool preserve_arguments)
{
    namespace fs = std::filesystem;
    const auto command = recognize_launcher(context.launcher.script_body);
    if (!command || !context.launch_environment) throw std::runtime_error("unsupported_launcher");
    addon::Process_proposal result;
    if (context.launcher.runner_key == "direct" && cli_argument_index(context.launcher_argv)) {
        result.executable = addon::Process_executable::launcher_program();
        result.argv = preserve_arguments
            ? std::vector<std::string>(context.launcher_argv.begin() + 1, context.launcher_argv.end())
            : command->prefix_arguments;
        return result;
    }
    if (preserve_arguments) throw std::runtime_error("unsupported_launcher");
    const auto environment = addon::environment_pairs(context.launch_environment->effective_environment);
    const bool windows = context.launch_environment->platform_id == "windows-x86_64";
    std::vector<fs::path> candidates;
    auto program = addon::path_from_utf8(command->program);
    if (program.is_absolute() || program.has_parent_path()) {
        candidates.push_back(program.is_absolute() ? program : addon::path_from_utf8(context.working_directory) / program);
    }
    else {
        std::istringstream directories(addon::environment_value(environment, "PATH"));
        for (std::string directory; std::getline(directories, directory, windows ? ';' : ':');) {
            if (directory.empty()) continue;
            auto path = addon::path_from_utf8(directory);
            if (!path.is_absolute()) path = addon::path_from_utf8(context.working_directory) / path;
            if (path.is_absolute()) candidates.push_back(path / program);
        }
    }
    std::optional<fs::path> resolved;
    for (const auto& candidate : candidates) {
        auto extensions = windows && candidate.extension().empty()
            ? std::vector<std::string>{".exe", ".cmd", ".bat"} : std::vector<std::string>{""};
        for (const auto& extension : extensions) {
            std::error_code error;
            auto path = candidate;
            path += extension;
            if (fs::is_regular_file(path, error) && !error) {
                resolved = fs::absolute(path).lexically_normal();
                break;
            }
        }
        if (resolved) break;
    }
    if (!resolved) throw std::runtime_error("launcher_program_unavailable");
#ifdef _WIN32
    result.executable = "helpers/claude_launch_bridge.exe";
#else
    result.executable = "helpers/claude_launch_bridge";
#endif
    result.argv.push_back(path_string(*resolved));
    result.argv.insert(result.argv.end(), command->prefix_arguments.begin(), command->prefix_arguments.end());
    return result;
}

std::vector<logonomic::cli_addon::Launcher_candidate> discover_launchers(
    const logonomic::cli_addon::Discover_request& request)
{
    namespace fs = std::filesystem;
    const auto path = text(request.ambient_environment, "PATH");
    const bool windows = request.platform_id == "windows-x86_64";
    std::istringstream directories(path);
    std::string directory;
    while (std::getline(directories, directory, windows ? ';' : ':')) {
        if (directory.empty()) {
            continue;
        }
        for (const auto& suffix : windows ? std::vector<std::string>{".exe", ".cmd", ".ps1", ".bat"}
                                         : std::vector<std::string>{""})
        {
            const auto candidate = addon::path_from_utf8(directory) / ("claude" + suffix);
            std::error_code error;
            if (fs::is_regular_file(candidate, error) && !error) {
                std::vector<addon::Launcher_candidate> launchers;
                auto selected = text(request.ambient_environment, "CLAUDE_CONFIG_DIR");
                if (selected.empty()) {
                    auto home = text(request.ambient_environment, windows ? "USERPROFILE" : "HOME");
                    if (home.empty()) home = request.default_home_fallback;
                    if (!home.empty()) selected = path_string(addon::path_from_utf8(home) / ".claude");
                }
                std::vector<fs::path> profiles;
                std::set<std::string> canonical_profiles;
                std::optional<std::string> selected_profile_identity;
                const auto selected_path = addon::path_from_utf8(selected);
                const auto add_profile = [&](const fs::path& profile) {
                    const std::string profile_path = path_string(profile.lexically_normal());
                    const std::string environment_text = "CLAUDE_CONFIG_DIR=" + profile_path;
                    if (!is_valid_claude_config_environment_text(environment_text)) {
                        return;
                    }
                    std::error_code canonical_error;
                    const auto canonical = fs::canonical(profile, canonical_error);
                    if (canonical_error) {
                        return;
                    }
                    const std::string identity = path_string(canonical);
                    if (!canonical_profiles.insert(identity).second) {
                        return;
                    }
                    profiles.push_back(profile);
                    if (profile == selected_path) {
                        selected_profile_identity = identity;
                    }
                };
                if (selected_path.is_absolute()) {
                    // A profile that cannot be checked, such as one on an unreachable
                    // share, is not a candidate: it must not fail the discovery of
                    // the launcher. A folder that cannot be read is skipped.
                    std::error_code profile_error;
                    if (fs::is_regular_file(selected_path / ".credentials.json", profile_error)) {
                        add_profile(selected_path);
                    }
                    const auto parent = selected_path.parent_path();
                    if (fs::is_directory(parent, profile_error)) {
                        std::size_t scanned_entries = 0;
                        for (fs::directory_iterator entry(parent, profile_error), end;
                            !profile_error && entry != end &&
                            scanned_entries < k_max_profile_directory_entries_to_scan;
                            entry.increment(profile_error), ++scanned_entries)
                        {
                            const auto profile = entry->path();
                            std::error_code entry_error;
                            if (entry->is_directory(entry_error) && path_string(profile.filename()).starts_with(".claude") &&
                                fs::is_regular_file(profile / ".credentials.json", entry_error))
                            {
                                add_profile(profile);
                                if (profiles.size() == addon::limits::k_max_discovery_candidates) {
                                    break;
                                }
                            }
                        }
                    }
                }
                for (const auto& profile : profiles) {
                    if (launchers.size() == addon::limits::k_max_discovery_candidates) {
                        break;
                    }
                    const std::string profile_path = path_string(profile.lexically_normal());
                    const std::string environment_text = "CLAUDE_CONFIG_DIR=" + profile_path;
                    if (!is_valid_claude_config_environment_text(environment_text)) continue;
                    std::error_code canonical_error;
                    const auto canonical = fs::canonical(profile, canonical_error);
                    if (canonical_error) continue;
                    const bool is_selected = selected_profile_identity &&
                        path_string(canonical) == *selected_profile_identity;
                    std::string profile_label = path_string(profile.filename());
                    if (profile_label.starts_with(".claude")) profile_label.erase(0, 7);
                    while (!profile_label.empty() &&
                        (profile_label.front() == '-' || profile_label.front() == '_' || profile_label.front() == '.'))
                    {
                        profile_label.erase(0, 1);
                    }
                    for (char& ch : profile_label) {
                        if (ch == '-' || ch == '_' || ch == '.') ch = ' ';
                    }
                    launchers.push_back({"profile_" + account_key(profile).substr(8, 24),
                        bounded_display_name(profile_label.empty() ? "Claude" : "Claude " + profile_label),
                        "claude", windows ? "cmd" : "direct",
                        environment_text, is_selected, addon::Json_value::object()});
                }
                if (launchers.empty()) launchers.push_back({"interactive", "Claude", "claude", windows ? "cmd" : "direct",
                    "", true, addon::Json_value::object()});
                return launchers;
            }
        }
    }
    return {};
}

} // namespace claude
