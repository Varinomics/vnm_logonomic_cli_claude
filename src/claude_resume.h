#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/session_contracts.h>

#include <string_view>
#include <string>
#include <vector>

namespace claude {

struct conversation_state_t
{
    logonomic::cli_addon::Resume_availability availability = logonomic::cli_addon::Resume_availability::UNKNOWN;
    logonomic::cli_addon::Resume_content      content = logonomic::cli_addon::Resume_content::UNKNOWN;
};

conversation_state_t inspect_saved_conversation(
    const logonomic::cli_addon::Session_launch_context& launch,
    std::string_view                                   session_id,
    const std::vector<std::string>&                    command,
    logonomic::cli_addon::Invocation_context&          invocation);

} // namespace claude
