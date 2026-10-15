#include "claude_quota_direct.h"
#include "claude_support.h"

#include <logonomic_cli_addon/launcher_utilities.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <ctime>
#include <sys/utsname.h>
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace claude {
namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
using Json = addon::Json_value;

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
    const auto size = fs::file_size(path, error);
    if (error || size > addon::Json_parse_limits{}.max_bytes) return std::nullopt;
    std::ifstream input(path, std::ios::binary);
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!input || input.peek() != std::char_traits<char>::eof()) return std::nullopt;
    return bytes;
}

bool absent(const fs::path& path)
{
    std::error_code error;
    const bool present = fs::exists(path, error);
    return !error && !present;
}

std::optional<Json> read_object(const fs::path& path, bool required = false)
{
    if (!required && absent(path)) return Json::object();
    auto bytes = read_file(path);
    if (!bytes) return std::nullopt;
    auto value = addon::parse_json(*bytes, addon::Json_parse_limits{}, nullptr);
    std::fill(bytes->begin(), bytes->end(), '\0');
    if (!value || !value->is_object()) return std::nullopt;
    return value;
}

bool native_image(const fs::path& path)
{
#ifdef __APPLE__
    return addon::native_image(path);
#else
    std::ifstream input(path, std::ios::binary);
    std::array<unsigned char, 4> magic{};
    input.read(reinterpret_cast<char*>(magic.data()), magic.size());
#ifdef _WIN32
    return input && magic[0] == 'M' && magic[1] == 'Z';
#else
    return input && magic == std::array<unsigned char, 4>{0x7f, 'E', 'L', 'F'};
#endif
#endif
}

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

bool quota_arguments(const std::vector<std::string>& arguments)
{
    const std::array<std::string_view, 16> expected{
        "-p", "/usage", "--output-format", "json", "--verbose", "--no-session-persistence",
        "--safe-mode", "--strict-mcp-config", "--mcp-config", "", "--tools", "", "--allowedTools", "", "--model", "sonnet"};
    if (arguments.size() != expected.size()) return false;
    for (std::size_t index = 0; index < expected.size(); ++index) {
        if (index != 9 && arguments[index] != expected[index]) return false;
    }
    const auto mcp = read_object(addon::path_from_utf8(arguments[9]), true);
    const auto* servers = mcp ? mcp->find("mcpServers") : nullptr;
    return mcp && mcp->as_object().size() == 1 && servers && servers->is_object() && servers->as_object().empty();
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

bool configuration_supported(const fs::path& profile, const fs::path& working_directory,
    const Quota_environment& environment)
{
    if (!profile.is_absolute() || !working_directory.is_absolute() || !policy_absent() ||
        !absent(profile / ".config.json") || !absent(profile / "remote-settings.json"))
    {
        return false;
    }
    // XI()/s7() consider these CCR handoff files even without an inherited
    // descriptor selector. Native Windows resolves the rooted path on cwd's
    // drive; Linux uses the filesystem root.
    if (!absent(working_directory.root_path() / "home/claude/.claude/remote")) return false;
    const auto user = read_object(profile / "settings.json");
    if (!user || !quota_settings_supported(*user)) return false;
    // Native project/local settings can change env and authentication. Checking
    // every ancestor also covers a different native Git-root selection.
    for (auto directory = working_directory; !directory.empty();) {
        // POSIX native localSettings can select a linked worktree's main
        // checkout outside these ancestors. Do not guess that consent root.
        if (!absent(directory / ".git")) {
            std::error_code error;
            const auto status = fs::symlink_status(directory / ".git", error);
            if (error || !fs::is_directory(status)) return false;
        }
        for (const auto* name : {"settings.json", "settings.local.json"}) {
            const auto settings = read_object(directory / ".claude" / name);
            if (!settings || !quota_settings_supported(*settings)) return false;
        }
        const auto parent = directory.parent_path();
        if (parent == directory) break;
        directory = parent;
    }
#ifdef _WIN32
    const auto config = addon::environment_value(environment, "APPDATA");
    if (config.empty()) return false;
    return absent(addon::path_from_utf8(config) / "Anthropic");
#else
    auto config = addon::environment_value(environment, "XDG_CONFIG_HOME");
    if (config.empty()) {
        const auto home = addon::environment_value(environment, "HOME");
        if (home.empty()) return false;
        config = path_string(addon::path_from_utf8(home) / ".config");
    }
    return absent(addon::path_from_utf8(config) / "anthropic");
#endif
}

std::optional<std::chrono::sys_seconds> parse_reset(std::string_view text)
{
    // The endpoint's ISO timestamp carries its own offset. Admit that shape
    // instead of guessing dates from locale-dependent text.
    if (text.size() < 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
        text[13] != ':' || text[16] != ':') return std::nullopt;
    const auto number = [&](std::size_t offset, std::size_t count) -> std::optional<int> {
        int value = 0;
        const auto parsed = std::from_chars(text.data() + offset, text.data() + offset + count, value);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + offset + count) return std::nullopt;
        return value;
    };
    const auto year   = number(0, 4);
    const auto month  = number(5, 2);
    const auto day    = number(8, 2);
    const auto hour   = number(11, 2);
    const auto minute = number(14, 2);
    const auto second = number(17, 2);
    if (!year || !month || !day || !hour || !minute || !second || *hour > 23 || *minute > 59 || *second > 59)
        return std::nullopt;
    const std::chrono::year_month_day date{std::chrono::year(*year), std::chrono::month(*month), std::chrono::day(*day)};
    if (!date.ok()) return std::nullopt;
    std::size_t offset = 19;
    if (text[offset] == '.') {
        ++offset;
        const auto start = offset;
        while (offset < text.size() && text[offset] >= '0' && text[offset] <= '9') ++offset;
        if (offset == start) return std::nullopt;
    }
    int zone_minutes = 0;
    if (text.substr(offset) != "Z") {
        if (text.size() != offset + 6 || (text[offset] != '+' && text[offset] != '-') || text[offset + 3] != ':')
            return std::nullopt;
        const auto hours = number(offset + 1, 2);
        const auto minutes = number(offset + 4, 2);
        if (!hours || !minutes || *hours > 23 || *minutes > 59) return std::nullopt;
        zone_minutes = (*hours * 60 + *minutes) * (text[offset] == '+' ? 1 : -1);
    }
    return std::chrono::sys_days(date) + std::chrono::hours(*hour) + std::chrono::minutes(*minute - zone_minutes) +
        std::chrono::seconds(*second);
}

std::string reset_text(std::chrono::sys_seconds instant, std::uint64_t now)
{
    using namespace std::chrono;
    local_seconds local, current;
    std::string zone;
#ifdef _WIN32
    const auto* timezone = current_zone();
    local   = timezone->to_local(instant);
    current = timezone->to_local(sys_seconds(seconds(now / 1000)));
    zone    = timezone->name();
#else
    const auto convert = [](sys_seconds point) {
        const auto stamp = system_clock::to_time_t(point);
        std::tm value{};
        if (!localtime_r(&stamp, &value)) throw std::runtime_error("quota_timezone_unavailable");
        return local_days(year(value.tm_year + 1900) / month(value.tm_mon + 1) / day(value.tm_mday)) +
            hours(value.tm_hour) + minutes(value.tm_min) + seconds(value.tm_sec);
    };
    local   = convert(instant);
    current = convert(sys_seconds(seconds(now / 1000)));
    const auto path = fs::canonical("/etc/localtime");
    const auto spelling = path_string(path);
    const auto begin = spelling.find("/zoneinfo/");
    if (begin == std::string::npos) throw std::runtime_error("quota_timezone_unavailable");
    zone = spelling.substr(begin + 10);
#endif
    if (zone == "Etc/UTC" || zone == "Etc/GMT") zone = "UTC";
    const year_month_day date{floor<days>(local)};
    const year_month_day today{floor<days>(current)};
    const hh_mm_ss time{local - floor<days>(local)};
    constexpr std::array months{"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    std::string result = std::string(months[unsigned(date.month()) - 1]) + ' ' + std::to_string(unsigned(date.day()));
    if (date.year() != today.year()) result += ", " + std::to_string(int(date.year()));
    const auto hour = time.hours().count();
    result += ", " + std::to_string(hour % 12 == 0 ? 12 : hour % 12);
    if (time.minutes().count() != 0) {
        result += time.minutes().count() < 10 ? ":0" : ":";
        result += std::to_string(time.minutes().count());
    }
    return result + (hour < 12 ? "am (" : "pm (") + zone + ')';
}

} // namespace

bool quota_environment_supported(const Quota_environment& environment)
{
    for (const auto& [name, value] : environment) {
        (void)value;
        const auto key = upper(name);
        if ((key.starts_with("CLAUDE_") && key != "CLAUDE_CONFIG_DIR") || key.starts_with("_CLAUDE_") ||
            key.starts_with("ANTHROPIC_") || (key.starts_with("GIT_") && key != "GIT_PAGER") || key.starts_with("WSL") ||
            key.starts_with("NODE_") || key.starts_with("BUN_") || key == "HTTP_PROXY" || key == "HTTPS_PROXY" ||
            key == "ALL_PROXY" || key == "NO_PROXY" || key == "SSL_CERT_FILE" || key == "SSL_CERT_DIR" ||
            key == "CURL_CA_BUNDLE" || key == "TZ") return false;
    }
    return true;
}

bool quota_settings_supported(const Json& settings)
{
    if (!settings.is_object()) return false;
    // These settings do not select credentials, endpoints, policies or network
    // transport. Customizations listed here are disabled by the existing
    // --safe-mode invocation. Unknown configuration stays with the native CLI.
    constexpr std::array<std::string_view, 30> supported{
        "$schema", "attribution", "permissions", "model", "enabledPlugins", "extraKnownMarketplaces",
        "alwaysThinkingEnabled", "effortLevel", "modelSettings", "autoUpdatesChannel", "tui", "voice",
        "skipDangerousModePermissionPrompt", "theme", "autoCompactEnabled", "switchModelsOnFlag",
        "agentPushNotifEnabled", "autoMode", "voiceEnabled", "feedbackDrafts", "hooks", "env",
        "cleanupPeriodDays", "includeCoAuthoredBy", "language", "outputStyle", "statusLine",
        "enableAllProjectMcpServers", "enabledMcpjsonServers", "disabledMcpjsonServers"};
    for (const auto& [name, value] : settings.as_object()) {
        if (std::find(supported.begin(), supported.end(), name) == supported.end()) return false;
        if (name == "env" && (!value.is_object() || !value.as_object().empty())) return false;
    }
    return true;
}

std::optional<std::string> current_quota_access_token(
    const Json& credentials, const Json& config, std::uint64_t now, std::uint64_t remaining_ms)
{
    const auto* oauth = credentials.find("claudeAiOauth");
    if (!credentials.is_object() || credentials.as_object().size() != 1 || !oauth || !oauth->is_object() ||
        !config.is_object() || config.find("primaryApiKey")) return std::nullopt;
    if (const auto* features = config.find("cachedGrowthBookFeatures")) {
        if (!features->is_object()) return std::nullopt;
        // zn() otherwise selects CredMan before the plaintext fallback.
        if (const auto* credman = features->find("tengu_windows_credman");
            credman && (!credman->is_boolean() || credman->as_boolean())) return std::nullopt;
    }
    const auto type = text(*oauth, "subscriptionType");
    if (type != "pro" && type != "max") return std::nullopt;
    const auto* scopes = oauth->find("scopes");
    const auto* expiry = oauth->find("expiresAt");
    const auto bearer = text(*oauth, "accessToken");
    if (!scopes || !scopes->is_array() || !expiry || !expiry->is_number() || bearer.empty() ||
        !std::isfinite(expiry->as_number()) || !std::all_of(bearer.begin(), bearer.end(),
            [](unsigned char byte) { return byte >= 0x21 && byte <= 0x7e; })) return std::nullopt;
    const auto has_scope = [&](std::string_view scope) {
        return std::any_of(scopes->as_array().begin(), scopes->as_array().end(),
            [&](const Json& value) { return value.is_string() && value.as_string() == scope; });
    };
    if (!has_scope("user:inference") || !has_scope("user:profile")) return std::nullopt;
    // Native jN() renews at now + 300000 >= expiresAt. Reserve the remaining
    // observation budget too; the native CLI alone owns refresh and rotation.
    if (expiry->as_number() - static_cast<double>(now) <= 300000.0 + static_cast<double>(remaining_ms))
        return std::nullopt;
    return bearer;
}

std::optional<std::string> quota_usage_output(const Json& response, std::uint64_t now)
{
    if (!response.is_object() || response.find("error")) return std::nullopt;
    std::string content;
    for (const auto& [key, title] : {std::pair{"five_hour", "Current session"},
        {"seven_day", "Current week (all models)"}})
    {
        const auto* window = response.find(key);
        const auto* used = window ? window->find("utilization") : nullptr;
        if (!window || !window->is_object() || !used || !used->is_number() ||
            !std::isfinite(used->as_number()) || used->as_number() < 0) return std::nullopt;
        if (!content.empty()) content += '\n';
        content += std::string(title) + ": " + std::to_string(std::floor(used->as_number())) + "% used";
        if (const auto* reset = window->find("resets_at"); reset && !reset->is_null()) {
            if (!reset->is_string()) return std::nullopt;
            const auto parsed = parse_reset(reset->as_string());
            if (!parsed) return std::nullopt;
            content += " · resets " + reset_text(*parsed, now);
        }
    }
    auto result = Json::object();
    result.set("type", Json("result"));
    result.set("subtype", Json("success"));
    result.set("local_command", Json("usage"));
    result.set("is_error", Json(false));
    result.set("result", Json(std::move(content)));
    return addon::serialize_json(result) + '\n';
}

std::optional<std::string> direct_quota_output(
    const fs::path& program, const std::vector<std::string>& arguments,
    const Quota_environment& environment, const fs::path& working_directory,
    Quota_clock::time_point deadline, Quota_fetch fetch)
{
    try {
        if (!quota_environment_supported(environment) || !quota_arguments(arguments) || !selected_native_program(program))
            return std::nullopt;
        const auto profile_name = addon::environment_value(environment, "CLAUDE_CONFIG_DIR");
        // Native Se() normalizes this path to NFC. ASCII has the same spelling
        // on both paths; other spellings retain native normalization authority.
        if (!std::all_of(profile_name.begin(), profile_name.end(), [](unsigned char byte) { return byte < 0x80; }))
            return std::nullopt;
        const auto profile = addon::path_from_utf8(profile_name);
        if (!configuration_supported(profile, working_directory, environment)) return std::nullopt;
        const auto config = read_object(profile / ".claude.json");
        const auto credentials = read_object(profile / ".credentials.json", true);
        if (!config || !credentials) return std::nullopt;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Quota_clock::now()).count();
        if (remaining <= 0) return std::nullopt;
        auto bearer = current_quota_access_token(*credentials, *config, now_ms(), static_cast<std::uint64_t>(remaining));
        if (!bearer) return std::nullopt;
        const auto bytes = fetch(*bearer, deadline);
        std::fill(bearer->begin(), bearer->end(), '\0');
        if (!bytes || Quota_clock::now() >= deadline) return std::nullopt;
        const auto current = read_object(profile / ".credentials.json", true);
        if (!current || addon::serialize_json(*current) != addon::serialize_json(*credentials)) return std::nullopt;
        const auto response = addon::parse_json(*bytes, addon::Json_parse_limits{}, nullptr);
        return response ? quota_usage_output(*response, now_ms()) : std::nullopt;
    }
    catch (const std::exception&) {
        // Unsupported files, storage, timezones and replies remain native.
        // Exception text can contain paths or provider data and is never logged.
        return std::nullopt;
    }
}

} // namespace claude
