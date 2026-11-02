#include "claude_cost.h"
#include "claude_accounts.h"
#include "claude_support.h"

#include <logonomic_cli_addon/json.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace claude {
namespace {

namespace addon = logonomic::cli_addon;
namespace fs    = std::filesystem;
using Json      = addon::Json_value;
using Clock     = std::chrono::steady_clock;

constexpr std::size_t k_max_receipt_bytes = 64 * 1024;
constexpr auto        k_read_budget       = std::chrono::milliseconds(500);

struct fingerprint_t
{
    std::uintmax_t     size;
    fs::file_time_type modified;

    bool operator==(const fingerprint_t&) const = default;
};

void check_budget(const addon::Invocation_context& context, Clock::time_point deadline)
{
    if (context.cancel_requested()) {
        throw std::runtime_error("cancelled");
    }
    if (Clock::now() >= deadline) {
        throw std::runtime_error("cost_read_timed_out");
    }
}

fingerprint_t fingerprint(const fs::path& path)
{
    return {fs::file_size(path), fs::last_write_time(path)};
}

double amount(const Json* value)
{
    if (!value || !value->is_number() || !std::isfinite(value->as_number()) || value->as_number() < 0) {
        throw std::runtime_error("cost_amount_invalid");
    }
    return value->as_number();
}

Json object(std::string_view bytes)
{
    addon::Json_parse_limits limits;
    limits.max_bytes        = k_max_receipt_bytes;
    limits.max_string_bytes = k_max_receipt_bytes;
    auto value = addon::parse_json(bytes, limits, nullptr);
    if (!value || !value->is_object()) {
        throw std::runtime_error("cost_record_invalid");
    }
    return std::move(*value);
}

} // namespace

std::optional<addon::Quota_cost> read_recorded_cost(
    const addon::Account_context& account, addon::Invocation_context& context)
{
    const auto deadline = Clock::now() + k_read_budget;
    check_budget(context, deadline);
    const auto profile = profile_for_account(account);
    if (!fs::is_directory(profile)) {
        throw std::runtime_error("cost_profile_unavailable");
    }
    const auto directory = profile / "logonomic-cost-receipts";
    check_budget(context, deadline);
    const bool present = fs::exists(directory);
    check_budget(context, deadline);
    if (!present) {
        return std::nullopt;
    }
    // Native cost-state snapshots contain price estimates without a historical
    // payer. Only result receipts establish API billing for each saved run.
    std::optional<double> total;
    for (const auto& entry : fs::directory_iterator(directory)) {
        check_budget(context, deadline);
        if (!entry.is_regular_file() || entry.path().extension() != ".json") {
            continue;
        }
        const auto before = fingerprint(entry.path());
        if (before.size > k_max_receipt_bytes) {
            throw std::runtime_error("cost_receipt_capacity");
        }
        std::ifstream file(entry.path(), std::ios::binary);
        std::string bytes((std::size_t)before.size, '\0');
        if (!file || !file.read(bytes.data(), (std::streamsize)bytes.size())) {
            throw std::runtime_error("cost_source_unreadable");
        }
        check_budget(context, deadline);
        if (fingerprint(entry.path()) != before) {
            throw std::runtime_error("cost_source_changed");
        }
        const auto record   = object(bytes);
        const auto* version = record.find("version");
        const auto* instant = record.find("recorded_at_ms");
        const auto session  = text(record, "session_id");
        const auto basis    = text(record, "cost_basis");
        const auto selected = addon::path_from_utf8(text(record, "profile_path"));
        if (!version || !version->is_integer() || version->as_integer() != 1 ||
            text(record, "source") != "claude-code-result" || text(record, "billing") != "api" ||
            session.empty() || session != path_string(entry.path().stem()) ||
            !instant || !instant->is_integer() || instant->as_integer() <= 0 ||
            (basis != "list" && basis != "managed" && basis != "unknown" && basis != "mixed"))
        {
            throw std::runtime_error("cost_receipt_invalid");
        }
        if (!selected.is_absolute() || account_key(selected) != account.adapter_account_key) {
            throw std::runtime_error("cost_receipt_profile_mismatch");
        }
        total = total.value_or(0) + amount(record.find("total_cost_usd"));
        if (!std::isfinite(*total)) {
            throw std::runtime_error("cost_amount_invalid");
        }
    }
    // A cancelled or exhausted optional read never publishes a partial sum.
    check_budget(context, deadline);
    if (!total) {
        return std::nullopt;
    }
    return addon::Quota_cost{*total, "USD", true, "Recorded API runs", std::nullopt};
}

} // namespace claude
