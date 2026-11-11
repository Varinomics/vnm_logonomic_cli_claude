#include "claude_native_context.h"
#include "claude_filesystem.h"
#include "claude_support.h"

#include <logonomic_cli_addon/launcher_utilities.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/utsname.h>
#endif

#include <algorithm>
#include <array>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

namespace claude::native_context {
namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
using Json = addon::Json_value;

namespace {

std::string upper(std::string value)
{
    for (auto& byte : value) {
        if (byte >= 'a' && byte <= 'z') byte -= 'a' - 'A';
    }
    return value;
}

std::optional<std::string> read_file(const fs::path& path)
{
    std::error_code error;
    const auto io_path = native_io_path(path);
    const auto size = fs::file_size(io_path, error);
    if (error || size > addon::Json_parse_limits{}.max_bytes) return std::nullopt;
    std::ifstream input(io_path, std::ios::binary);
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!input || input.peek() != std::char_traits<char>::eof()) return std::nullopt;
    return bytes;
}

} // namespace

bool absent(const fs::path& path)
{
    std::error_code error;
    const bool present = fs::exists(native_io_path(path), error);
    return !error && !present;
}

std::optional<Json> read_object(const fs::path& path, bool required)
{
    if (!required && absent(path)) return Json::object();
    auto bytes = read_file(path);
    if (!bytes) return std::nullopt;
    auto value = addon::parse_json(*bytes, addon::Json_parse_limits{}, nullptr);
    std::fill(bytes->begin(), bytes->end(), '\0');
    if (!value || !value->is_object()) return std::nullopt;
    return value;
}

namespace {

bool native_image(const fs::path& path)
{
#ifdef __APPLE__
    return addon::native_image(path);
#else
    std::ifstream input(native_io_path(path), std::ios::binary);
    std::array<unsigned char, 4> magic{};
    input.read(reinterpret_cast<char*>(magic.data()), magic.size());
#ifdef _WIN32
    return input && magic[0] == 'M' && magic[1] == 'Z';
#else
    return input && magic == std::array<unsigned char, 4>{0x7f, 'E', 'L', 'F'};
#endif
#endif
}

} // namespace

bool selected_native_program(const fs::path& program)
{
    // Admit the native npm installation selected by the launcher. A same-name
    // executable outside that installation may itself be a context-changing
    // wrapper, just like a script. Other install layouts remain native.
    const auto name = upper(path_string(program.filename()));
    const auto packaged_native = [](const fs::path& executable) {
        const auto package = executable.parent_path().parent_path();
        const auto manifest = read_object(package / "package.json", true);
        const auto* bin = manifest ? manifest->find("bin") : nullptr;
        const auto relative = executable.lexically_relative(package).generic_u8string();
        return manifest && text(*manifest, "name") == "@anthropic-ai/claude-code" && bin &&
            text(*bin, "claude") == std::string(relative.begin(), relative.end()) && native_image(executable);
    };
    if (name == "CLAUDE" || name == "CLAUDE.EXE") return packaged_native(program);
#ifdef _WIN32
    if (name != "CLAUDE.CMD") return false;
    const auto bytes = read_file(program);
    if (!bytes) return false;
    std::istringstream input(*bytes);
    std::string normalized;
    for (std::string line; std::getline(input, line);) {
        std::istringstream words(line);
        std::string word;
        bool first = true;
        while (words >> word) {
            if (!first) normalized += ' ';
            normalized += upper(word);
            first = false;
        }
        if (!first) normalized += '\n';
    }
    constexpr auto stock =
        "@ECHO OFF\nGOTO START\n:FIND_DP0\nSET DP0=%~DP0\nEXIT /B\n:START\nSETLOCAL\nCALL :FIND_DP0\n"
        "\"%DP0%\\NODE_MODULES\\@ANTHROPIC-AI\\CLAUDE-CODE\\BIN\\CLAUDE.EXE\" %*\n";
    if (normalized != stock) return false;
    const auto package = program.parent_path() / "node_modules/@anthropic-ai/claude-code";
    return packaged_native(package / "bin/claude.exe");
#else
    return false;
#endif
}

bool policy_absent()
{
#ifdef _WIN32
    for (const auto hive : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
        DWORD size = 0;
        const auto status = RegGetValueW(hive, L"SOFTWARE\\Policies\\ClaudeCode", L"Settings",
            RRF_RT_ANY | RRF_SUBKEY_WOW6464KEY, nullptr, nullptr, &size);
        if (status != ERROR_FILE_NOT_FOUND && status != ERROR_PATH_NOT_FOUND) return false;
    }
    // Native yy() uses this path literally, independent of ProgramFiles.
    const fs::path directory = "C:/Program Files/ClaudeCode";
#else
    // WSL can inherit the Windows policy chain before /etc/claude-code. Leave
    // that cross-platform authority selection with the native CLI.
    utsname kernel{};
    if (uname(&kernel) != 0 || upper(kernel.release).find("MICROSOFT") != std::string::npos) return false;
    const fs::path directory = "/etc/claude-code";
#endif
    return absent(directory / "managed-settings.json") && absent(directory / "managed-settings.d");
}

} // namespace claude::native_context
