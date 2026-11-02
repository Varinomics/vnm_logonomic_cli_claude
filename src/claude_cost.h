#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/quota.h>

#include <optional>

namespace claude {

// Local recorded session estimates, without a claim about historical payer,
// billing period, credit balance, or sessions whose costs were not saved.
// Missing records return nothing; unreadable or conflicting evidence throws.
std::optional<logonomic::cli_addon::Quota_cost> read_recorded_cost(
    const logonomic::cli_addon::Account_context& account,
    logonomic::cli_addon::Invocation_context& context);

} // namespace claude
