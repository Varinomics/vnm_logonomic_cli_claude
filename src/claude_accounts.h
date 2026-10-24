#pragma once

#include <logonomic_cli_addon/accounts.h>
#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/session_contracts.h>

#include <filesystem>
#include <string>

namespace claude {

std::string account_key(const std::filesystem::path& profile);
// The profile an accounts@2 context names, refused unless its key is that profile's.
std::filesystem::path profile_for_account(const logonomic::cli_addon::Account_context& account);
std::filesystem::path profile_for_context(const logonomic::cli_addon::Session_launch_context& context);
void apply_account(const logonomic::cli_addon::Session_launch_context& context,
    logonomic::cli_addon::Process_proposal& process);
void handle_accounts(logonomic::cli_addon::Invocation_context& context);

} // namespace claude
