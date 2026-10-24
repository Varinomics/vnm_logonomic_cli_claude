#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/session_contracts.h>

#include <string_view>
#include <string>
#include <vector>

namespace claude {

logonomic::cli_addon::Resume_availability saved_conversation_availability(
    const logonomic::cli_addon::Session_launch_context& launch,
    std::string_view                                   session_id,
    const std::vector<std::string>&                    command,
    logonomic::cli_addon::Invocation_context&          invocation);

} // namespace claude
