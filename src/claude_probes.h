#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/auth_status.h>
#include <logonomic_cli_addon/quota.h>

#include <filesystem>

namespace claude {

logonomic::cli_addon::Auth_status inspect_auth(const logonomic::cli_addon::Structured_inspection& inspection);
logonomic::cli_addon::Quota_inspect_result inspect_quota(const logonomic::cli_addon::Structured_inspection& inspection);
void handle_probes(logonomic::cli_addon::Invocation_context& context, const std::filesystem::path& package_root);

} // namespace claude
