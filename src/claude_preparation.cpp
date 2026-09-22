#include "claude_preparation.h"
#include "claude_accounts.h"
#include "claude_launcher.h"
#include "claude_lifecycle.h"
#include "claude_support.h"

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
    const std::optional<std::string>& plugin, const std::vector<std::string>& trailing)
{
    if (instruction.find_first_not_of(" \t\r\n") == std::string::npos) throw std::runtime_error("title_instruction_missing");
    auto command = literal_command(argv);
    if (!command) return std::nullopt;
    const auto cli_index = *cli_argument_index(*command);
    if (plugin) {
        std::optional<std::string> last;
        for (std::size_t index = cli_index + 1; index < command->size(); ++index) {
            if ((*command)[index] == "--plugin-dir" && index + 1 < command->size()) last = (*command)[++index];
            else
            if ((*command)[index].starts_with("--plugin-dir=")) last = (*command)[index].substr(13);
        }
        if (!last || *last != *plugin) command->insert(command->end(), {"--plugin-dir", *plugin});
    }
    bool present = false;
    for (std::size_t index = cli_index + 1; index < command->size(); ++index) {
        auto& argument = (*command)[index];
        if (argument == "--append-system-prompt") {
            if (index + 1 == command->size()) command->push_back(instruction);
            else
            if ((*command)[index + 1].find(instruction) == std::string::npos) (*command)[index + 1] += ' ' + instruction;
            present = true;
            break;
        }
        if (argument.starts_with("--append-system-prompt=")) {
            if (argument.find(instruction) == std::string::npos) argument += ' ' + instruction;
            present = true;
            break;
        }
    }
    if (!present) command->insert(command->begin() + static_cast<std::ptrdiff_t>(cli_index + 1), {"--append-system-prompt", instruction});
    command->insert(command->end(), trailing.begin(), trailing.end());
    if (cli_argument_index(argv)) return command;
    auto output = argv;
    const auto index = *shell_command_index(argv);
    const auto shell = executable_name(argv.front());
    if (shell == "cmd") {
        output.resize(index);
        output.insert(output.end(), command->begin(), command->end());
        return output;
    }
    if (command->front().find_first_of(" \t") != std::string::npos) return std::nullopt;
    const bool powershell = shell == "powershell" || shell == "pwsh";
    std::string rendered = command->front();
    for (std::size_t arg = 1; arg < command->size(); ++arg) rendered += ' ' + shell_quote((*command)[arg], powershell);
    output[index] = std::move(rendered);
    return output;
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
        const auto argv = prepared_argv(input->launcher_argv, input->title_instruction,
            hooks ? std::optional(path_string(package_root / "assets/claude-hooks")) : std::nullopt, {});
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
    const auto argv = prepared_argv(input->context.launcher_argv, input->title_instruction,
        path_string(package_root / "assets/claude-hooks"), {"--resume", session});
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
    result.patch.set.emplace_back("CLAUDE_CODE_SESSIONEND_HOOKS_TIMEOUT_MS", "5000");
    result.acknowledgment.record_digest_hex = addon::resume_record_digest_hex(input->resume_record);
    context.succeed(addon::encode_resume_prepare_result(result, 2));
}

} // namespace claude
