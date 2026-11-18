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

std::optional<Launcher_command> recognize_launcher(std::string_view script, std::string_view runner);
std::optional<std::size_t> cli_argument_index(const std::vector<std::string>& argv);
struct Claude_option_span
{
    // Commander's remaining token after expanding leading short switches.
    // This may be a synthetic --. The caller keeps the original argv word.
    std::string argument;
    std::size_t value_words = 0;
};

Claude_option_span claude_option_span(const std::vector<std::string>& argv, std::size_t index);
// The words that follow argv[index] as its value under Claude's native option
// grammar, including required, optional and variadic values. Attached values,
// switches and unknown options consume no following words. This is arity only;
// admission to an interactive resume is a separate policy.
std::size_t claude_option_value_words(const std::vector<std::string>& argv, std::size_t index);
// On refusal, reports the option name without an inline value when requested.
bool interactive_resume_arguments(const std::vector<std::string>& argv, std::string* refused_option = nullptr);
bool direct_implementation_arguments(const logonomic::cli_addon::Session_launch_context& context);
logonomic::cli_addon::Process_proposal cli_process(
    const logonomic::cli_addon::Session_launch_context& context, bool preserve_arguments);
std::vector<logonomic::cli_addon::Launcher_candidate> discover_launchers(
    const logonomic::cli_addon::Discover_request& request);

} // namespace claude
