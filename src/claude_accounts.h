#pragma once

#include <logonomic_cli_addon/accounts.h>
#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/session_contracts.h>

#include <filesystem>
#include <string>

namespace claude {

std::string account_key(const std::filesystem::path& profile);
std::filesystem::path profile_for_context(const logonomic::cli_addon::Session_launch_context& context);
void apply_account(const logonomic::cli_addon::Session_launch_context& context,
    logonomic::cli_addon::Process_proposal& process);
void handle_accounts(logonomic::cli_addon::Invocation_context& context);
void handle_history(logonomic::cli_addon::Invocation_context& context);

} // namespace claude
