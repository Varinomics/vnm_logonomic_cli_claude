#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/quota.h>

#include <optional>

namespace claude {

// API-billed session estimates from profile-bound result receipts, without a
// claim about billing period, credit balance, or runs whose costs were not saved.
// Native snapshots do not establish billing. Missing receipts return nothing;
// unreadable or incomplete evidence throws without publishing a partial total.
std::optional<logonomic::cli_addon::Quota_cost> read_recorded_cost(
    const logonomic::cli_addon::Account_context& account,
    logonomic::cli_addon::Invocation_context& context);

} // namespace claude
