#include "claude_quota_windows.h"
#include "claude_support.h"

#include <charconv>
#include <chrono>
#include <cmath>
#include <string>
#include <string_view>

namespace claude {
namespace {

namespace addon = logonomic::cli_addon;
using Json = addon::Json_value;

std::optional<std::uint64_t> reset_milliseconds(const Json& reset)
{
    constexpr double max_exact_integer = 9007199254740991.0;
    if (reset.is_number()) {
        const double seconds = reset.as_number();
        if (!std::isfinite(seconds) || seconds < 0 || std::floor(seconds) != seconds ||
            seconds > max_exact_integer / 1000)
        {
            return std::nullopt;
        }
        return (std::uint64_t)seconds * 1000;
    }
    if (!reset.is_string()) {
        return std::nullopt;
    }
    const std::string_view value = reset.as_string();
    if (value.size() < 20 || value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
        value[13] != ':' || value[16] != ':')
    {
        return std::nullopt;
    }
    const auto number = [&](std::size_t offset, std::size_t count) -> std::optional<int> {
        for (const char digit : value.substr(offset, count)) {
            if (digit < '0' || digit > '9') {
                return std::nullopt;
            }
        }
        int result = 0;
        const auto parsed = std::from_chars(value.data() + offset, value.data() + offset + count, result);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + offset + count) {
            return std::nullopt;
        }
        return result;
    };
    const auto year   = number(0, 4);
    const auto month  = number(5, 2);
    const auto day    = number(8, 2);
    const auto hour   = number(11, 2);
    const auto minute = number(14, 2);
    const auto second = number(17, 2);
    if (!year || !month || !day || !hour || !minute || !second || *hour > 23 || *minute > 59 || *second > 59) {
        return std::nullopt;
    }
    const std::chrono::year_month_day date{
        std::chrono::year(*year), std::chrono::month(*month), std::chrono::day(*day)};
    if (!date.ok()) {
        return std::nullopt;
    }
    std::size_t offset = 19;
    int milliseconds = 0;
    if (value[offset] == '.') {
        const auto begin = ++offset;
        while (offset < value.size() && value[offset] >= '0' && value[offset] <= '9') {
            if (offset - begin < 3) {
                milliseconds = milliseconds * 10 + value[offset] - '0';
            }
            ++offset;
        }
        if (offset == begin) {
            return std::nullopt;
        }
        for (auto digits = offset - begin; digits < 3; ++digits) {
            milliseconds *= 10;
        }
    }
    int zone_minutes = 0;
    if (value.substr(offset) != "Z") {
        if (value.size() != offset + 6 || (value[offset] != '+' && value[offset] != '-') ||
            value[offset + 3] != ':')
        {
            return std::nullopt;
        }
        const auto hours   = number(offset + 1, 2);
        const auto minutes = number(offset + 4, 2);
        if (!hours || !minutes || *hours > 23 || *minutes > 59) {
            return std::nullopt;
        }
        zone_minutes = (*hours * 60 + *minutes) * (value[offset] == '+' ? 1 : -1);
    }
    const auto instant = std::chrono::sys_days(date) + std::chrono::hours(*hour) +
        std::chrono::minutes(*minute - zone_minutes) + std::chrono::seconds(*second) +
        std::chrono::milliseconds(milliseconds);
    const auto count = std::chrono::duration_cast<std::chrono::milliseconds>(instant.time_since_epoch()).count();
    return count >= 0 ? std::optional<std::uint64_t>((std::uint64_t)count) : std::nullopt;
}

std::string readable_name(std::string value)
{
    for (auto& character : value) {
        if (character == '_' || character == '-') {
            character = ' ';
        }
    }
    if (!value.empty() && value.front() >= 'a' && value.front() <= 'z') {
        value.front() -= 'a' - 'A';
    }
    return value;
}

std::optional<std::string> window_label(const Json& row)
{
    auto label = text(row, "label");
    const auto kind = text(row, "kind");
    bool all_models = false;
    if (label.empty()) {
        if (kind == "session" || kind == "five_hour") {
            label = "Current session";
        }
        else
        if (kind == "weekly_all" || kind == "seven_day") {
            label = "Current week";
            all_models = true;
        }
        else
        if (kind == "weekly_scoped") {
            label = "Current week";
        }
        else {
            label = readable_name(kind);
            const auto group = text(row, "group");
            if (!group.empty() && !kind.empty() && kind != group &&
                !kind.starts_with(group + '_') && !kind.starts_with(group + '-'))
            {
                label = readable_name(group) + ": " + label;
            }
        }
    }
    if (label.empty()) {
        return std::nullopt;
    }
    std::string names;
    if (const auto* scope = row.find("scope"); scope && !scope->is_null()) {
        if (!scope->is_object()) {
            return std::nullopt;
        }
        for (const auto& [dimension, value] : scope->as_object()) {
            if (value.is_null()) {
                continue;
            }
            const auto name = value.is_string() ? value.as_string() : text(value, "display_name");
            if (name.empty()) {
                return std::nullopt;
            }
            if (dimension == "model") {
                all_models = false;
            }
            if (!names.empty()) {
                names += ", ";
            }
            names += dimension == "model" || dimension == "surface"
                ? name : readable_name(dimension) + ": " + name;
        }
    }
    if (all_models) {
        names = names.empty() ? "all models" : "all models, " + names;
    }
    if (!names.empty()) {
        label += " (" + names + ')';
    }
    return label;
}

} // namespace

std::optional<std::vector<addon::Quota_window>> parse_quota_windows(const Json& limits)
{
    if (!limits.is_array()) {
        return std::nullopt;
    }
    std::vector<addon::Quota_window> windows;
    for (const auto& row : limits.as_array()) {
        const auto* percent = row.find("percent");
        if (!row.is_object() || !percent || !percent->is_number() ||
            !std::isfinite(percent->as_number()) || percent->as_number() < 0)
        {
            return std::nullopt;
        }
        addon::Quota_window window;
        window.used_percent = percent->as_number();
        window.label = window_label(row);
        if (!window.label) {
            return std::nullopt;
        }
        if (const auto* reset = row.find("resets_at"); reset && !reset->is_null()) {
            window.resets_at_ms = reset_milliseconds(*reset);
            if (!window.resets_at_ms) {
                return std::nullopt;
            }
        }
        if (const auto* headline = row.find("is_active")) {
            if (!headline->is_boolean()) {
                return std::nullopt;
            }
            // Claude's is_active picks the headline meter; false does not remove a limit.
            window.headline = headline->as_boolean();
        }
        windows.push_back(std::move(window));
    }
    return windows;
}

std::optional<Json> quota_response_limits(const Json& response)
{
    if (!response.is_object() || response.find("error")) {
        return std::nullopt;
    }
    if (const auto* limits = response.find("limits"); limits && !limits->is_null()) {
        // An empty server collection is authoritative; named legacy fields cannot revive it.
        return parse_quota_windows(*limits) ? std::optional<Json>(*limits) : std::nullopt;
    }
    auto limits = Json::array();
    bool observed_window = false;
    for (const auto& [name, value] : response.as_object()) {
        const auto* percent = value.find("utilization");
        if (!value.is_object() || !percent) {
            continue;
        }
        observed_window = true;
        const auto* enabled = value.find("is_enabled");
        if (percent->is_null() || (enabled && enabled->is_boolean() && !enabled->as_boolean())) {
            continue;
        }
        auto row = Json::object();
        row.set("kind", Json(name));
        row.set("percent", *percent);
        for (const auto* member : {"label", "scope", "resets_at"}) {
            if (const auto* metadata = value.find(member)) {
                row.set(member, *metadata);
            }
        }
        limits.push_back(std::move(row));
    }
    if (!observed_window || !parse_quota_windows(limits)) {
        return std::nullopt;
    }
    return limits;
}

} // namespace claude
