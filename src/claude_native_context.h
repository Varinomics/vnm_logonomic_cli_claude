#pragma once

#include <logonomic_cli_addon/json.h>

#include <filesystem>
#include <optional>

namespace claude::native_context {

bool absent(const std::filesystem::path& path);
std::optional<logonomic::cli_addon::Json_value> read_object(
    const std::filesystem::path& path, bool required = false);
bool selected_native_program(const std::filesystem::path& program);
bool policy_absent();

} // namespace claude::native_context
