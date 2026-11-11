#pragma once

#include <logonomic_cli_addon/usage_scan.h>

#include <filesystem>

namespace claude {

inline std::filesystem::path native_io_path(const std::filesystem::path& path)
{
#ifdef _WIN32
    // Extended Windows paths require dot segments to be resolved before I/O.
    return logonomic::cli_addon::usage_cache_io_path(path.lexically_normal());
#else
    return path;
#endif
}

} // namespace claude
