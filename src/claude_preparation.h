#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/launcher_discovery.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace claude {

bool node_available(const logonomic::cli_addon::Launch_environment& launch, const std::string& working_directory);

std::optional<std::vector<std::string>> prepared_argv(
    const std::vector<std::string>& argv, const std::string& instruction,
    const std::vector<std::string>& plugins, const std::vector<std::string>& trailing);
void handle_preparation(logonomic::cli_addon::Invocation_context& context, const std::filesystem::path& package_root);

} // namespace claude
