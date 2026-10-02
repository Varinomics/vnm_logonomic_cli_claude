#pragma once

#include "claude_quota_direct.h"

namespace claude {

std::optional<std::string> get_subscription_quota(std::string_view bearer, Quota_clock::time_point deadline);

} // namespace claude
