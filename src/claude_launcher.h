#pragma once

#include <logonomic_cli_addon/launcher_discovery.h>
#include <logonomic_cli_addon/session_contracts.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace claude {

struct Launcher_command
{
    std::string                                      program;
    std::vector<std::string>                         prefix_arguments;
    std::vector<std::pair<std::string, std::string>> environment;
};

std::optional<Launcher_command> recognize_launcher(std::string_view script);
std::optional<std::size_t> cli_argument_index(const std::vector<std::string>& argv);
// The words that follow argv[index] as its value when Claude parses argv, for
// the options an interactive launch may carry: one after an option that takes
// a value, none after a switch, an option=value word or an unknown option.
std::size_t claude_option_value_words(const std::vector<std::string>& argv, std::size_t index);
bool interactive_resume_arguments(const std::vector<std::string>& argv);
bool direct_implementation_arguments(const logonomic::cli_addon::Session_launch_context& context);
logonomic::cli_addon::Process_proposal cli_process(
    const logonomic::cli_addon::Session_launch_context& context, bool preserve_arguments);
std::vector<logonomic::cli_addon::Launcher_candidate> discover_launchers(
    const logonomic::cli_addon::Discover_request& request);

} // namespace claude
