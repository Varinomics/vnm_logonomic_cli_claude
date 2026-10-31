#pragma once

#include <logonomic_cli_addon/json.h>
#include <logonomic_cli_addon/quota.h>

#include <optional>
#include <vector>

namespace claude {

std::optional<std::vector<logonomic::cli_addon::Quota_window>> parse_quota_windows(
    const logonomic::cli_addon::Json_value& limits);
std::optional<logonomic::cli_addon::Json_value> quota_response_limits(
    const logonomic::cli_addon::Json_value& response);

} // namespace claude
