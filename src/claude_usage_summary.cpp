#include "claude_usage_summary.h"
#include "claude_accounts.h"

#include <logonomic_cli_addon/json.h>
#include <logonomic_cli_addon/sha256.h>
#include <logonomic_cli_addon/usage_lifetime_summary.h>
#include <logonomic_cli_addon/usage_saved_daily_tokens.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace claude {
namespace {

namespace addon = logonomic::cli_addon;
namespace fs    = std::filesystem;
using Json      = addon::Json_value;

constexpr std::int64_t   k_max_exact       = 9007199254740991;
constexpr std::uintmax_t k_max_cache_bytes = 512 * 1024 * 1024;

// Claude Code's saved totals in <profile>/stats-cache.json, with the source
// metadata both saved-usage capabilities report.
struct Stats_cache
{
    Json                        source;
    std::string                 digest;
    std::optional<std::int64_t> format_version;
    std::optional<std::string>  stated_date;
};

void stop_if_cancelled(const addon::Invocation_context& context)
{
    if (context.cancel_requested()) throw std::runtime_error("cancelled");
}

// A counter or version the SDK can carry exactly: a JSON integer in
// [0, 2^53 - 1]. Every contract limit on the results, such as distinct model
// rows and the checked sum of all counters, is enforced by validating the
// outgoing payload with the SDK's own decoder.
std::int64_t exact_integer(const Json* value)
{
    if (!value || !value->is_integer() || value->as_integer() < 0 || value->as_integer() > k_max_exact) {
        throw std::runtime_error("stats_cache_integer_invalid");
    }
    return value->as_integer();
}

std::string read_bytes(const fs::path& path, const addon::Invocation_context& context)
{
    const auto size     = fs::file_size(path);
    const auto modified = fs::last_write_time(path);
    if (size > k_max_cache_bytes) throw std::runtime_error("stats_cache_capacity");
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("stats_cache_unreadable");
    std::string bytes(static_cast<std::size_t>(size), '\0');
    for (std::size_t offset = 0; offset < bytes.size();) {
        stop_if_cancelled(context);
        const auto count = std::min<std::size_t>(64 * 1024, bytes.size() - offset);
        if (!file.read(bytes.data() + offset, static_cast<std::streamsize>(count))) {
            throw std::runtime_error("stats_cache_unreadable");
        }
        offset += count;
    }
    // Claude Code can rewrite the cache at any time; a read that overlapped a
    // rewrite is not a snapshot of either version.
    char extra;
    if (file.get(extra) || file.bad() || fs::file_size(path) != size || fs::last_write_time(path) != modified) {
        throw std::runtime_error("stats_cache_changed");
    }
    return bytes;
}

// The stats cache of the selected profile only, or nothing when that profile
// has none.
std::optional<Stats_cache> read_stats_cache(
    const addon::Account_context& account, const addon::Invocation_context& context)
{
    const auto path = profile_for_account(account) / "stats-cache.json";
    std::error_code error;
    if (!fs::exists(path, error) && !error) return std::nullopt;
    if (error || !fs::is_regular_file(path)) throw std::runtime_error("stats_cache_unreadable");
    const auto bytes = read_bytes(path, context);

    addon::Json_parse_limits limits;
    limits.max_bytes = k_max_cache_bytes;
    addon::Json_parse_error parse_error;
    auto source = addon::parse_json(bytes, limits, &parse_error);
    if (!source || !source->is_object()) throw std::runtime_error("stats_cache_invalid");

    Stats_cache cache;
    cache.digest = addon::to_hex(addon::Sha256::digest(bytes));
    if (const auto* version = source->find("version")) cache.format_version = exact_integer(version);
    if (const auto* date = source->find("lastComputedDate")) {
        if (!date->is_string()) throw std::runtime_error("stats_cache_date_invalid");
        cache.stated_date = date->as_string();
    }
    cache.source = std::move(*source);
    return cache;
}

addon::Usage_lifetime_summary lifetime_summary(const addon::Account_context& account, const Stats_cache& cache)
{
    const auto* models = cache.source.find("modelUsage");
    if (!models || !models->is_object()) throw std::runtime_error("stats_cache_models_invalid");

    addon::Usage_lifetime_summary summary;
    summary.account_context       = account;
    summary.source_kind           = "claude-stats-cache";
    summary.source_content_digest = cache.digest;
    summary.source_format_version = cache.format_version;
    summary.cache_stated_date     = cache.stated_date;
    for (const auto& member : models->as_object()) {
        const auto counter = [&member](std::string_view name) {
            return static_cast<std::uint64_t>(exact_integer(member.value.find(name)));
        };
        summary.models.push_back({
            member.name,
            counter("inputTokens"),
            counter("outputTokens"),
            counter("cacheReadInputTokens"),
            counter("cacheCreationInputTokens"),
        });
    }
    std::sort(summary.models.begin(), summary.models.end(),
        [](const auto& left, const auto& right) { return left.model < right.model; });
    return summary;
}

// Nothing when the cache has no dated series, which leaves its lifetime
// totals readable.
std::optional<addon::Usage_saved_daily_tokens> saved_daily_tokens(
    const addon::Account_context& account, const Stats_cache& cache, const addon::Invocation_context& context)
{
    const auto* days = cache.source.find("dailyModelTokens");
    if (!days) return std::nullopt;
    if (!days->is_array()) throw std::runtime_error("stats_cache_daily_invalid");

    addon::Usage_saved_daily_tokens summary;
    summary.account_context       = account;
    summary.source_kind           = "claude-stats-cache";
    summary.source_content_digest = cache.digest;
    summary.source_format_version = cache.format_version;
    summary.cache_stated_date     = cache.stated_date;
    if (const auto* version = cache.source.find("dailyModelTokensVersion")) {
        summary.daily_series_format_version = exact_integer(version);
    }
    for (const Json& source_day : days->as_array()) {
        stop_if_cancelled(context);
        const auto* date   = source_day.find("date");
        const auto* models = source_day.find("tokensByModel");
        if (!date || !date->is_string() || !models || !models->is_object()) {
            throw std::runtime_error("stats_cache_daily_invalid");
        }
        addon::Usage_saved_daily_day day;
        day.date = date->as_string();
        for (const auto& member : models->as_object()) {
            day.models.push_back({member.name, static_cast<std::uint64_t>(exact_integer(&member.value))});
        }
        std::sort(day.models.begin(), day.models.end(),
            [](const auto& left, const auto& right) { return left.model < right.model; });
        summary.days.push_back(std::move(day));
    }
    std::sort(summary.days.begin(), summary.days.end(),
        [](const auto& left, const auto& right) { return left.date < right.date; });
    return summary;
}

} // namespace

void handle_usage_summary(addon::Invocation_context& context)
{
    if (context.request().operation != addon::k_usage_lifetime_summary_read_operation) {
        context.reject_unknown_operation();
        return;
    }
    addon::Contract_failure failure;
    const auto request = addon::decode_usage_lifetime_summary_request(context.request().payload, &failure);
    if (!request) {
        context.fail(addon::Result_status::FAILED, failure.reason_code);
        return;
    }
    const auto cache = read_stats_cache(request->account_context, context);
    if (!cache) {
        context.fail(addon::Result_status::UNAVAILABLE, "stats_cache_unavailable");
        return;
    }
    const auto payload = addon::encode_usage_lifetime_summary_result(
        lifetime_summary(request->account_context, *cache));
    if (!addon::decode_usage_lifetime_summary_result(payload, &failure)) {
        throw std::runtime_error("stats_cache_invalid");
    }
    stop_if_cancelled(context);
    context.succeed(payload);
}

void handle_usage_daily(addon::Invocation_context& context)
{
    if (context.request().operation != addon::k_usage_saved_daily_tokens_read_operation) {
        context.reject_unknown_operation();
        return;
    }
    addon::Contract_failure failure;
    const auto request = addon::decode_usage_saved_daily_tokens_request(context.request().payload, &failure);
    if (!request) {
        context.fail(addon::Result_status::FAILED, failure.reason_code);
        return;
    }
    const auto cache = read_stats_cache(request->account_context, context);
    if (!cache) {
        context.fail(addon::Result_status::UNAVAILABLE, "stats_cache_unavailable");
        return;
    }
    const auto summary = saved_daily_tokens(request->account_context, *cache, context);
    if (!summary) {
        context.fail(addon::Result_status::UNAVAILABLE, "stats_cache_daily_unavailable");
        return;
    }
    const auto payload = addon::encode_usage_saved_daily_tokens_result(*summary);
    if (!addon::decode_usage_saved_daily_tokens_result(payload, &failure)) {
        throw std::runtime_error("stats_cache_daily_invalid");
    }
    stop_if_cancelled(context);
    context.succeed(payload);
}

} // namespace claude
