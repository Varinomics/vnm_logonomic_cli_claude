#pragma once

#include <logonomic_cli_addon/json.h>
#include <logonomic_cli_addon/package_source.h>

#include <chrono>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace claude {

inline std::string text(const logonomic::cli_addon::Json_value& object, std::string_view name)
{
    const auto* value = object.find(name);
    return value && value->is_string() ? value->as_string() : std::string{};
}

inline bool flag(const logonomic::cli_addon::Json_value& object, std::string_view name)
{
    const auto* value = object.find(name);
    return value && value->is_boolean() && value->as_boolean();
}

inline logonomic::cli_addon::Json_value object(std::string_view bytes)
{
    namespace addon = logonomic::cli_addon;
    addon::Json_parse_error error;
    auto value = addon::parse_json(bytes, addon::Json_parse_limits{}, &error);
    if (!value || !value->is_object()) {
        throw std::runtime_error("invalid_json_object");
    }
    return std::move(*value);
}

inline logonomic::cli_addon::Json_value strings(const std::vector<std::string>& values)
{
    auto result = logonomic::cli_addon::Json_value::array();
    for (const auto& value : values) {
        result.push_back(logonomic::cli_addon::Json_value(value));
    }
    return result;
}

inline std::string path_string(const std::filesystem::path& value)
{
    const auto bytes = value.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

inline std::uint64_t now_ms()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

} // namespace claude
