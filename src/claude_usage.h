#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/usage_scan.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace claude {

struct Usage_session
{
    logonomic::cli_addon::Usage_record record;
    std::array<std::uint64_t, 4>       category_values{};
    std::array<std::uint64_t, 4>       category_coverage{};
    std::uint64_t                      message_count = 0;
    std::map<std::string, std::uint64_t> repository_tokens;
    std::uint64_t                      unattributed_tokens = 0;
    std::map<std::string, std::uint64_t> temporal_tokens;
    std::uint64_t                      temporal_unavailable_tokens = 0;
    std::size_t                        temporal_position_bytes = 0;
    bool                               temporal_capacity = false;
    bool                               temporal_missing = false;
    bool                               temporal_unsupported = false;
    bool                               temporal_retained = false;
};

struct Usage_cache_input
{
    std::optional<std::filesystem::path> native_seed;
    std::string seed_sha256_hex;
    bool cache_required = false;
    std::optional<std::filesystem::path> storage_file;
    std::optional<std::filesystem::path> selected_file;
    // usage-scan's incremental mode: reuse a transcript's cached messages
    // while its size and modification time are unchanged. Repair and reset
    // parse every transcript.
    bool incremental = false;
};

std::vector<Usage_session> scan_usage(
    const std::filesystem::path& profile, logonomic::cli_addon::Invocation_context& context,
    const Usage_cache_input& cache = {});
void handle_usage(logonomic::cli_addon::Invocation_context& context);

} // namespace claude
