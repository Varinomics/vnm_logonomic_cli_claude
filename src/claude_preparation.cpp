#include "claude_preparation.h"
#include "claude_wake.h"
#include "claude_accounts.h"
#include "claude_launcher.h"
#include "claude_lifecycle.h"
#include "claude_support.h"

#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/launch_preparation.h>
#include <logonomic_cli_addon/launcher_utilities.h>
#include <logonomic_cli_addon/session_contracts.h>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace claude {
namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

// cmd.exe reads at most 8191 characters of a command line. An npm .cmd shim
// repeats Claude's arguments on a line of its own behind its quoted program
// paths, which cmd.exe keeps below MAX_PATH each; 1024 characters cover them,
// and the program word a cmd launcher names itself.
constexpr std::size_t k_cmd_line_characters   = 8191;
constexpr std::size_t k_cmd_program_allowance = 1024;

std::string executable_name(std::string value)
{
    const auto slash = value.find_last_of("/\\");
    if (slash != std::string::npos) value.erase(0, slash + 1);
    for (char& ch : value) {
        if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    }
    if (value.ends_with(".exe")) value.resize(value.size() - 4);
    return value;
}

std::optional<std::vector<std::string>> shell_words(std::string_view command, bool powershell)
{
    std::vector<std::string> result;
    std::string current;
    char quote = 0;
    bool started = false;
    for (std::size_t index = 0; index < command.size(); ++index) {
        const char ch = command[index];
        if (!quote) {
            if (std::isspace(static_cast<unsigned char>(ch))) {
                if (started) {
                    result.push_back(current);
                    current.clear();
                    started = false;
                }
                continue;
            }
            if (ch == ';' || ch == '|' || ch == '<' || ch == '>' || ch == '&' || ch == '`' || ch == '$') {
                return std::nullopt;
            }
            if (!powershell && ch == '\\' && index + 1 < command.size()) {
                current += command[++index];
                started = true;
                continue;
            }
            if (ch == '\'' || ch == '"') {
                quote = ch;
                started = true;
                continue;
            }
            current += ch;
            started = true;
            continue;
        }
        if (ch == quote) {
            if (powershell && index + 1 < command.size() && command[index + 1] == quote) {
                current += quote;
                ++index;
            }
            else quote = 0;
            continue;
        }
        if (quote == '"' && (ch == '$' || ch == '`')) return std::nullopt;
        if (!powershell && quote == '"' && ch == '\\' && index + 1 < command.size() &&
            (command[index + 1] == quote || command[index + 1] == '\\'))
        {
            current += command[++index];
            continue;
        }
        current += ch;
    }
    if (quote) return std::nullopt;
    if (started) result.push_back(current);
    return result;
}

std::string shell_quote(std::string_view value, bool powershell)
{
    std::string result = "'";
    for (const char ch : value) {
        if (ch == '\'') result += powershell ? "''" : "'\\''";
        else result += ch;
    }
    return result + '\'';
}

std::optional<std::size_t> shell_command_index(const std::vector<std::string>& argv)
{
    if (argv.empty()) return std::nullopt;
    const auto shell = executable_name(argv.front());
    for (std::size_t index = 1; index + 1 < argv.size(); ++index) {
        auto flag = argv[index];
        for (char& ch : flag) {
            if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
        }
        if ((shell == "cmd" && (flag == "/c" || flag == "/k")) ||
            ((shell == "powershell" || shell == "pwsh") && flag == "-command") ||
            ((shell == "bash" || shell == "sh") && (flag == "-c" || flag == "-lc")))
        {
            return index + 1 == argv.size() - 1 ? std::optional(index + 1) : std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<std::vector<std::string>> literal_command(const std::vector<std::string>& argv)
{
    if (cli_argument_index(argv)) return argv;
    const auto index = shell_command_index(argv);
    if (!index) return std::nullopt;
    const auto shell = executable_name(argv.front());
    auto parsed = shell == "cmd" ? std::optional(literal_argv(argv[*index]))
        : shell_words(argv[*index], shell == "powershell" || shell == "pwsh");
    if (!parsed || !cli_argument_index(*parsed)) return std::nullopt;
    return parsed;
}

void require_instruction(const std::string& instruction)
{
    if (instruction.find_first_not_of(" \t\r\n") == std::string::npos) {
        throw std::runtime_error("title_instruction_missing");
    }
}

// Claude's argument list for a launcher: its literal command with the hook
// plugin, the title instruction and the trailing arguments applied. The scans
// step over each option's value, so a value that reads like an option, as in
// --system-prompt --append-system-prompt, stays a value.
std::vector<std::string> claude_command(
    std::vector<std::string> command, const std::string& instruction,
    const std::vector<std::string>& plugins, const std::vector<std::string>& trailing)
{
    const auto cli_index = *cli_argument_index(command);
    const auto next_option = [&command](std::size_t index) {
        return index + 1 + claude_option_value_words(command, index);
    };
    for (const auto& plugin : plugins) {
        bool present = false;
        for (std::size_t index = cli_index + 1; index < command.size(); index = next_option(index)) {
            if ((command[index] == "--plugin-dir" && index + 1 < command.size() && command[index + 1] == plugin) ||
                (command[index].starts_with("--plugin-dir=") && command[index].substr(13) == plugin))
            {
                present = true;
                break;
            }
        }
        if (!present) command.insert(command.end(), {"--plugin-dir", plugin});
    }
    bool present = false;
    for (std::size_t index = cli_index + 1; index < command.size(); index = next_option(index)) {
        auto& argument = command[index];
        if (argument == "--append-system-prompt") {
            if (index + 1 == command.size()) command.push_back(instruction);
            else
            if (command[index + 1].find(instruction) == std::string::npos) command[index + 1] += ' ' + instruction;
            present = true;
            break;
        }
        if (argument.starts_with("--append-system-prompt=")) {
            if (argument.find(instruction) == std::string::npos) argument += ' ' + instruction;
            present = true;
            break;
        }
    }
    if (!present) command.insert(command.begin() + static_cast<std::ptrdiff_t>(cli_index + 1), {"--append-system-prompt", instruction});
    command.insert(command.end(), trailing.begin(), trailing.end());
    return command;
}

// The launcher argv that carries `command`, Claude's argument list taken from
// `argv`: the list itself, its words spliced behind cmd's /c or /k, or one
// quoted rendering in place of a shell's command string.
std::optional<std::vector<std::string>> carried_argv(
    const std::vector<std::string>& argv, std::vector<std::string> command)
{
    if (cli_argument_index(argv)) return command;
    auto output = argv;
    const auto index = *shell_command_index(argv);
    const auto shell = executable_name(argv.front());
    if (shell == "cmd") {
        output.resize(index);
        output.insert(output.end(), command.begin(), command.end());
        return output;
    }
    if (command.front().find_first_of(" \t") != std::string::npos) return std::nullopt;
    const bool powershell = shell == "powershell" || shell == "pwsh";
    std::string rendered = command.front();
    for (std::size_t arg = 1; arg < command.size(); ++arg) rendered += ' ' + shell_quote(command[arg], powershell);
    output[index] = std::move(rendered);
    return output;
}

// Whether Claude submits `text`, its positional prompt, unchanged. Claude reads
// an argument that starts with '-' as an option, and one word as a possible
// subcommand; it drops the prompt "code". It then submits the prompt as if the
// user had typed it, where input that trims to nothing is ignored, input that
// trims to exit, quit, :q, :q!, :wq or :wq! quits, input that starts with '/'
// runs a slash command, the word "ultraplan" turns the input into /ultraplan
// without that word, and a paste reference such as [Pasted text #1] or
// [Image #1] is replaced or dropped. Text of two or more words with visible
// ASCII characters at both ends, and without those forms, is submitted as it is.
bool verbatim_prompt(std::string_view text)
{
    const auto visible = [](char byte) { return byte > ' ' && byte < 0x7F; };
    if (text.find(' ') == std::string_view::npos || !visible(text.front()) || !visible(text.back()) ||
        text.front() == '-' || text.front() == '/')
    {
        return false;
    }
    std::string folded(text);
    for (char& byte : folded) {
        if (byte >= 'A' && byte <= 'Z') byte += 'a' - 'A';
    }
    for (const std::string_view form : {"ultraplan", "[pasted text #", "[image #", "[audio #", "[...truncated text #"}) {
        if (folded.find(form) != std::string::npos) {
            return false;
        }
    }
    return true;
}

// Whether the launcher's own arguments could put something before the first
// user message or keep it from being sent: --agent selects a main-thread
// agent, whose own initial prompt Claude prepends to the first user turn, and
// --settings with an inline JSON object, which Claude reads when the value,
// trimmed by JavaScript's trim(), starts with '{' and ends with '}', can select
// one or add a hook that blocks the message. The check trims ASCII whitespace
// and every non-ASCII character, a superset of what trim() removes. A settings
// file the launcher names is the user's configuration, like the user's own
// settings.
bool launcher_precedes_prompt(const std::vector<std::string>& command)
{
    const auto inline_json = [](std::string_view value) {
        const auto trimmed = [](char byte) {
            return static_cast<unsigned char>(byte) >= 0x80 || byte == ' ' || (byte >= '\t' && byte <= '\r');
        };
        while (!value.empty() && trimmed(value.front())) value.remove_prefix(1);
        while (!value.empty() && trimmed(value.back())) value.remove_suffix(1);
        return !value.empty() && value.front() == '{' && value.back() == '}';
    };
    for (std::size_t index = 1; index < command.size(); index += 1 + claude_option_value_words(command, index)) {
        const auto& argument = command[index];
        if (argument == "--agent" || argument.starts_with("--agent=") ||
            (argument == "--settings" && index + 1 < command.size() && inline_json(command[index + 1])) ||
            (argument.starts_with("--settings=") && inline_json(std::string_view(argument).substr(11))))
        {
            return true;
        }
    }
    return false;
}

// The directory holding the brief, the only one Claude is granted: never a
// root, and never read through . or .. segments, whose lexical parent could
// name a wider directory.
std::optional<std::string> brief_directory(const std::string& referenced_file)
{
    const auto file = addon::path_from_utf8(referenced_file);
    const auto directory = file.parent_path();
    if (!file.has_filename() || directory == directory.root_path()) {
        return std::nullopt;
    }
    for (const auto& segment : file) {
        if (segment == "." || segment == "..") {
            return std::nullopt;
        }
    }
    return path_string(directory);
}

// A Windows launch passes Claude's arguments through cmd.exe whenever Claude is
// an npm .cmd shim, and a cmd launcher carries them on its own /c line too.
// cmd.exe expands %variables% (and, when enabled, !variables!) even inside
// quotes, ends a line at a control character and has no escape for a quote
// inside a quoted argument; outside quotes it acts on & | < > ^ ( ). The host
// quotes an argument exactly when it holds a space.
bool cmd_literal(std::string_view argument)
{
    const bool control = std::any_of(argument.begin(), argument.end(),
        [](unsigned char byte) { return byte < 0x20 || byte == 0x7F; });
    return !control && argument.find_first_of("\"%!") == std::string_view::npos &&
        (argument.find(' ') != std::string_view::npos || argument.find_first_of("&|<>^()") == std::string_view::npos);
}

// An upper bound on the characters cmd.exe reads for Claude's arguments after
// the program: no UTF-8 text has fewer bytes than UTF-16 units, and the host
// adds a separator, the quotes of an argument with a space, and a doubled run
// of trailing backslashes before its closing quote.
std::size_t cmd_argument_characters(const std::vector<std::string>& command)
{
    std::size_t characters = 0;
    for (auto argument = command.begin() + 1; argument != command.end(); ++argument) {
        const auto kept = argument->find_last_not_of('\\');
        characters += 3 + argument->size() + (kept == std::string::npos ? argument->size() : argument->size() - kept - 1);
    }
    return characters;
}

// PowerShell also ends a single-quoted string at its four typographic single
// quotes, which the rendering does not double, and Windows PowerShell hands a
// native command an argument with whitespace inside double quotes without
// escaping an embedded quote or doubling a trailing backslash.
bool powershell_literal(std::string_view argument)
{
    for (const std::string_view quote : {"\"", "\xE2\x80\x98", "\xE2\x80\x99", "\xE2\x80\x9A", "\xE2\x80\x9B"}) {
        if (argument.find(quote) != std::string_view::npos) {
            return false;
        }
    }
    return !argument.ends_with('\\');
}

// The launcher argv that starts a new interactive Claude session whose first
// user message is `prompt.text` and that may read the brief's directory, or
// nullopt when this launcher cannot be prepared to deliver the text byte for
// byte.
std::optional<std::vector<std::string>> initial_prompt_argv(
    const std::vector<std::string>& argv, const std::string& instruction,
    const std::vector<std::string>& plugins, const addon::Initial_prompt_request& prompt, bool windows)
{
    require_instruction(instruction);
    // Claude itself, not a package runner that relaunches it through quoting of
    // its own, with only the known options that let exact resume append its
    // selector: the trailing argument stays the positional prompt, and no print
    // mode or session selector replaces the new interactive session.
    auto literal = literal_command(argv);
    if (!literal || cli_argument_index(*literal) != 0 || !interactive_resume_arguments(*literal) ||
        launcher_precedes_prompt(*literal))
    {
        return std::nullopt;
    }
    const auto directory = brief_directory(prompt.referenced_file);
    if (!directory || !verbatim_prompt(prompt.text)) {
        return std::nullopt;
    }
    auto command = claude_command(std::move(*literal), instruction, plugins, {prompt.text});
    // --add-dir takes every following word up to the next option, so it goes
    // directly behind the program, where an option always follows it.
    command.insert(command.begin() + 1, {"--add-dir", *directory});
    const auto shell = cli_argument_index(argv) ? std::string{} : executable_name(argv.front());
    if (windows) {
        // A POSIX shell on Windows converts path-like arguments (MSYS, Cygwin)
        // or runs Claude under WSL, where the brief's Windows path names nothing.
        if (shell == "bash" || shell == "sh") {
            return std::nullopt;
        }
        // cmd.exe strips the first and the last quote of a /c line that starts
        // with one. The line of a cmd launcher, and of a batch file the host
        // starts directly, begins with the program, quoted when it holds a space.
        const auto program = executable_name(command.front());
        const bool batch = shell.empty() && (program.ends_with(".cmd") || program.ends_with(".bat"));
        if ((shell == "cmd" || batch) && command.front().find(' ') != std::string::npos) {
            return std::nullopt;
        }
        if (!std::all_of(command.begin(), command.end(), cmd_literal) ||
            cmd_argument_characters(command) + k_cmd_program_allowance > k_cmd_line_characters)
        {
            return std::nullopt;
        }
    }
    if ((shell == "powershell" || shell == "pwsh") && !std::all_of(command.begin() + 1, command.end(), powershell_literal)) {
        return std::nullopt;
    }
    return carried_argv(argv, std::move(command));
}

} // namespace

bool node_available(const addon::Launch_environment& launch, const std::string& working_directory)
{
    const bool windows = launch.platform_id == "windows-x86_64";
    std::istringstream paths(addon::environment_value(addon::environment_pairs(launch.effective_environment), "PATH"));
    for (std::string directory; std::getline(paths, directory, windows ? ';' : ':');) {
        if (directory.empty()) continue;
        std::error_code error;
        auto path = addon::path_from_utf8(directory);
        if (!path.is_absolute()) path = addon::path_from_utf8(working_directory) / path;
        if (!path.is_absolute()) continue;
        if (fs::is_regular_file(path / (windows ? "node.exe" : "node"), error) && !error) return true;
    }
    return false;
}

bool canonical_resume_directory(const std::string& directory)
{
    const auto path = addon::path_from_utf8(directory);
    if (!path.is_absolute()) return false;
    auto normalized = path.lexically_normal();
    if (normalized != normalized.root_path() && normalized.filename().empty()) normalized = normalized.parent_path();
    const auto bytes = normalized.generic_u8string();
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()) == directory;
}

std::optional<std::vector<std::string>> prepared_argv(
    const std::vector<std::string>& argv, const std::string& instruction,
    const std::vector<std::string>& plugins, const std::vector<std::string>& trailing)
{
    require_instruction(instruction);
    auto command = literal_command(argv);
    if (!command) return std::nullopt;
    return carried_argv(argv, claude_command(std::move(*command), instruction, plugins, trailing));
}

void handle_preparation(addon::Invocation_context& context, const fs::path& package_root)
{
    addon::Contract_failure failure;
    if (context.request().capability.capability_id == "launch-preparation" && context.request().operation == "prepare") {
        const auto input = addon::decode_prepare_request(context.request().payload, &failure, 3);
        if (!input) {
            context.fail(addon::Result_status::FAILED, failure.reason_code);
            return;
        }
        const bool hooks = input->launch_environment && node_available(*input->launch_environment, input->working_directory);
        std::vector<std::string> plugins;
        if (hooks) {
            plugins.push_back(path_string(package_root / "assets/claude-hooks"));
            if (input->session_wake) {
                plugins.push_back(path_string(package_root / "assets/claude-wake-dispatch"));
            }
        }
        const auto argv = input->initial_prompt
            ? initial_prompt_argv(input->launcher_argv, input->title_instruction, plugins, *input->initial_prompt,
                  input->launch_environment->platform_id == "windows-x86_64")
            : prepared_argv(input->launcher_argv, input->title_instruction, plugins, {});
        if (input->initial_prompt && !argv) {
            context.fail(addon::Result_status::UNAVAILABLE, addon::reason::k_initial_prompt_unavailable);
            return;
        }
        addon::Launch_patch result;
        result.profile_storage = input->profile_storage;
        result.account_context = input->account_context;
        result.disposition = addon::Launch_disposition{argv.has_value(), argv.has_value() && hooks, argv.has_value() && hooks};
        if (argv) {
            result.argv = *argv;
            if (hooks) result.set.emplace_back("CLAUDE_CODE_SESSIONEND_HOOKS_TIMEOUT_MS", "5000");
            if (input->account_context) {
                addon::Session_launch_context launch;
                launch.account_context = input->account_context;
                launch.launch_environment = input->launch_environment;
                result.set.emplace_back("CLAUDE_CONFIG_DIR", path_string(profile_for_context(launch)));
            }
        }
        if (input->session_wake) prepare_wake(result, input->profile_storage, argv.has_value() && hooks, package_root);
        if (input->initial_prompt) {
            // The host holds a patch to the same bounds; one over them would
            // fail the launch after this package acknowledged the prompt.
            result.initial_prompt = addon::Initial_prompt_acknowledgment{
                addon::initial_prompt_text_digest_hex(input->initial_prompt->text)};
            if (!addon::validate_launch_patch(result, input->declared_assets, input->declared_assets, &failure)) {
                context.fail(addon::Result_status::UNAVAILABLE, addon::reason::k_initial_prompt_unavailable);
                return;
            }
        }
        context.succeed(addon::encode_launch_patch(result, 3));
        return;
    }
    if (context.request().capability.capability_id != "interactive-resume" || context.request().operation != "prepare_resume") {
        context.reject_unknown_operation();
        return;
    }
    const auto input = addon::decode_resume_prepare_request(context.request().payload, &failure, 2);
    if (!input) {
        context.fail(addon::Result_status::FAILED, failure.reason_code);
        return;
    }
    const auto session = text(input->resume_record, "provider_session_id");
    const auto literal = literal_command(input->context.launcher_argv);
    if (!input->resume_record.is_object() || input->resume_record.as_object().size() != 1 || !canonical_session_id(session) ||
        !literal || !interactive_resume_arguments(*literal) || !input->context.launch_environment ||
        !node_available(*input->context.launch_environment, input->context.working_directory) || !canonical_resume_directory(input->context.working_directory))
    {
        context.fail(addon::Result_status::UNAVAILABLE, "exact_resume_unavailable");
        return;
    }
    std::vector<std::string> plugins{path_string(package_root / "assets/claude-hooks")};
    if (input->context.session_wake) {
        plugins.push_back(path_string(package_root / "assets/claude-wake-dispatch"));
    }
    const auto argv = prepared_argv(input->context.launcher_argv, input->title_instruction,
        plugins, {"--resume", session});
    if (!argv) {
        context.fail(addon::Result_status::UNAVAILABLE, "unsupported_launcher");
        return;
    }
    addon::Resume_prepare_result result;
    result.process.executable = addon::Process_executable::launcher_program();
    result.process.argv.assign(argv->begin() + 1, argv->end());
    result.process.stdin_behavior = addon::Stdin_behavior::INHERIT;
    result.patch.set.emplace_back("CLAUDE_CONFIG_DIR", path_string(profile_for_context(input->context)));
    result.patch.profile_storage = input->context.profile_storage;
    result.patch.account_context = input->context.account_context;
    if (input->context.session_wake) prepare_wake(result.patch, input->context.profile_storage, true, package_root);
    result.patch.set.emplace_back("CLAUDE_CODE_SESSIONEND_HOOKS_TIMEOUT_MS", "5000");
    result.acknowledgment.record_digest_hex = addon::resume_record_digest_hex(input->resume_record);
    context.succeed(addon::encode_resume_prepare_result(result, 2));
}

} // namespace claude
