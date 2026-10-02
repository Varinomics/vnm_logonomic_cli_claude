#pragma once

#include <logonomic_cli_addon/json.h>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace claude {

using Quota_environment = std::vector<std::pair<std::string, std::string>>;
using Quota_clock = std::chrono::steady_clock;
using Quota_fetch = std::optional<std::string> (*)(std::string_view, Quota_clock::time_point);

bool quota_environment_supported(const Quota_environment& environment);
bool quota_settings_supported(const logonomic::cli_addon::Json_value& settings);
std::optional<std::string> current_quota_access_token(
    const logonomic::cli_addon::Json_value& credentials,
    const logonomic::cli_addon::Json_value& config,
    std::uint64_t now, std::uint64_t remaining_ms);
std::optional<std::string> quota_usage_output(
    const logonomic::cli_addon::Json_value& response, std::uint64_t now);

// Reads the same inherited context as the native fallback. It never refreshes,
// persists credentials, or sends a credential before admission is complete.
std::optional<std::string> direct_quota_output(
    const std::filesystem::path& program,
    const std::vector<std::string>& arguments,
    const Quota_environment& environment,
    const std::filesystem::path& working_directory,
    Quota_clock::time_point deadline,
    Quota_fetch fetch);

} // namespace claude
