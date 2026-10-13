#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/launch_preparation.h>

#include <optional>
#include <string>

namespace claude {

void prepare_wake(
    logonomic::cli_addon::Launch_patch& patch,
    const std::optional<logonomic::cli_addon::Profile_storage_context>& storage,
    bool supported_launch);

void handle_wake(logonomic::cli_addon::Invocation_context& context);
void register_wake(const logonomic::cli_addon::Json_value& event, const std::string& nonce);

} // namespace claude
