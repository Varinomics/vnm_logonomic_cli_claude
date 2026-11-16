#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/session_contracts.h>

#include <filesystem>

namespace claude {

logonomic::cli_addon::Structured_inspect_result inspect_session(
    logonomic::cli_addon::Structured_purpose purpose,
    const logonomic::cli_addon::Structured_inspection& inspection,
    bool observe_model = false);
void handle_sessions(logonomic::cli_addon::Invocation_context& context, const std::filesystem::path& package_root);

} // namespace claude
