#include "claude_quota_direct.h"
#include "claude_support.h"
#include "claude_native_context.h"
#include "claude_quota_windows.h"

#include <logonomic_cli_addon/launcher_utilities.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>

namespace claude {
namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
using Json = addon::Json_value;
using native_context::absent;
using native_context::policy_absent;
using native_context::read_object;
using native_context::selected_native_program;

std::string upper(std::string value)
{
    for (auto& byte : value) {
        if (byte >= 'a' && byte <= 'z') byte -= 'a' - 'A';
    }
    return value;
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

std::optional<std::string> quota_usage_output(const Json& response)
{
    const auto limits = quota_response_limits(response);
    if (!limits) {
        return std::nullopt;
    }
    auto rates = Json::object();
    rates.set("limits", *limits);
    auto report = Json::object();
    report.set("rate_limits", std::move(rates));
    auto result = Json::object();
    result.set("type", Json("assistant"));
    result.set("usage_report", std::move(report));
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
        return response ? quota_usage_output(*response) : std::nullopt;
    }
    catch (const std::exception&) {
        // Unsupported files, storage and replies remain native.
        // Exception text can contain paths or provider data and is never logged.
        return std::nullopt;
    }
}

} // namespace claude
