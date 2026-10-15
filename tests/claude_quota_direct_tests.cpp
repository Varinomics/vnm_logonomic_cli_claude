#include "claude_quota_direct.h"
#include "claude_probes.h"
#include "claude_support.h"

#include <logonomic_cli_addon/host_simulator.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#ifndef _WIN32
#include <sys/utsname.h>
#endif

namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
using Json = addon::Json_value;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

Json credentials(std::uint64_t expiry)
{
    auto value = claude::object(R"({"claudeAiOauth":{"accessToken":"fixture-token","refreshToken":"fixture-refresh","expiresAt":0,"scopes":["user:inference","user:profile"],"subscriptionType":"max"}})");
    value.find("claudeAiOauth")->set("expiresAt", Json(expiry));
    return value;
}

void admission_contract()
{
    require(claude::quota_environment_supported({{"CLAUDE_CONFIG_DIR", "/profile"}, {"HOME", "/home"}, {"GIT_PAGER", "cat"}}),
        "ordinary profile environment is eligible");
    for (const auto* key : {"ANTHROPIC_API_KEY", "CLAUDE_CODE_OAUTH_TOKEN", "CLAUDE_CODE_USE_BEDROCK",
        "CLAUDE_CODE_CUSTOM_OAUTH_URL", "CLAUDE_CODE_FORCE_WINDOWS_CREDMAN", "CLAUDE_CODE_PROCESS_WRAPPER",
        "_CLAUDE_CODE_ASSUME_FIRST_PARTY_BASE_URL", "GIT_DIR", "WSL_DISTRO_NAME",
        "https_proxy", "HTTPS_PROXY", "NODE_EXTRA_CA_CERTS", "SSL_CERT_FILE", "TZ"})
    {
        require(!claude::quota_environment_supported({{key, ""}}), "auth and transport selectors retain native authority");
    }
    require(claude::quota_settings_supported(claude::object(R"({"model":"sonnet","permissions":{},"hooks":{},"env":{}})")),
        "safe-mode customizations with no authority override are eligible");
    for (const auto* value : {R"({"env":{"ANTHROPIC_API_KEY":"fixture"}})", R"({"apiKeyHelper":"helper"})",
        R"({"forceLoginMethod":"gateway"})", R"({"processWrapper":"wrapper"})", R"({"unknownAuthSetting":true})"})
    {
        require(!claude::quota_settings_supported(claude::object(value)), "unresolved effective settings stay native");
    }
    constexpr std::uint64_t now = 1790899200000;
    auto record = credentials(now + 3600000);
    auto config = Json::object();
    require(claude::current_quota_access_token(record, config, now, 30000) == "fixture-token", "current subscription file token");
    require(!claude::current_quota_access_token(credentials(now + 330000), config, now, 30000),
        "native renewal threshold plus remaining budget is not eligible");
    require(claude::current_quota_access_token(credentials(now + 330001), config, now, 30000).has_value(),
        "token beyond the renewal boundary is eligible");
    config = claude::object(R"({"cachedGrowthBookFeatures":{"tengu_windows_credman":true}})");
    require(!claude::current_quota_access_token(record, config, now, 30000), "CredMan cannot be replaced by an unrelated file token");
    config = Json::object();
    record.find("claudeAiOauth")->set("subscriptionType", Json("team"));
    require(!claude::current_quota_access_token(record, config, now, 30000), "remote-policy eligible subscriptions remain native");
    record = credentials(now + 3600000);
    record.find("claudeAiOauth")->set("scopes", claude::strings({"user:inference"}));
    require(!claude::current_quota_access_token(record, config, now, 30000), "inference alone does not authorize a profile usage read");
    record = credentials(now + 3600000);
    record.set("gateway", Json::object());
    require(!claude::current_quota_access_token(record, config, now, 30000), "a competing credential store entry is not ignored");
}

void response_contract()
{
    constexpr std::uint64_t now = 1790899200000;
    const auto body = claude::object(R"({"five_hour":{"utilization":0,"resets_at":null},"seven_day":{"utilization":100.8,"resets_at":"2026-10-04T20:00:00.010907+00:00"}})");
    const auto output = claude::quota_usage_output(body, now);
    require(output.has_value(), "native ISO reset and explicit unknown primary reset are supported");
    addon::Structured_inspection input;
    input.final = true;
    input.stdout_bytes = *output;
    const auto observed = claude::inspect_quota(input);
    require(observed.state == addon::Quota_status::READY && observed.primary->used_percent == 0 &&
        !observed.primary->reset_text && observed.secondary->used_percent == 100 && observed.secondary->reset_text,
        "direct output uses native floor semantics and corrected optional resets");
    for (const auto* value : {"{}", R"({"error":{"type":"rate_limit_error"}})",
        R"({"five_hour":null,"seven_day":{"utilization":20}})",
        R"({"five_hour":{"utilization":null},"seven_day":{"utilization":20}})",
        R"({"five_hour":{"utilization":10},"seven_day":{"utilization":20,"resets_at":"tomorrow"}})"})
    {
        require(!claude::quota_usage_output(claude::object(value), now), "unusable replies defer to native cache/error semantics");
    }
    const auto unknown = claude::quota_usage_output(claude::object(
        R"({"five_hour":{"utilization":30,"resets_at":null},"seven_day":{"utilization":20,"resets_at":null}})"), now);
    require(unknown.has_value(), "both observed utilizations survive unknown resets");
}

int requests = 0;
enum class Reply { USAGE, ERROR, ROTATED_CREDENTIAL, LATE };
Reply reply = Reply::USAGE;
fs::path credential_path;
std::optional<std::string> fetch(std::string_view token, claude::Quota_clock::time_point deadline)
{
    ++requests;
    require(token == "fixture-token", "selected credential is used in memory");
    if (reply == Reply::ERROR) return std::nullopt;
    if (reply == Reply::ROTATED_CREDENTIAL) {
        std::ofstream file(credential_path);
        file << R"({"claudeAiOauth":{"accessToken":"rotated-fixture"}})";
    }
    if (reply == Reply::LATE) std::this_thread::sleep_until(deadline + std::chrono::milliseconds(20));
    return R"({"five_hour":{"utilization":10,"resets_at":null},"seven_day":{"utilization":25,"resets_at":null}})";
}

void filesystem_contract()
{
    const auto root = fs::temp_directory_path() / ("claude-quota-" + addon::random_nonce_hex());
    fs::create_directories(root / "profile");
    fs::create_directories(root / "work");
    fs::create_directories(root / "appdata");
    fs::create_directories(root / "program-files");
    fs::create_directories(root / "package/bin");
    const auto write = [](const fs::path& path, std::string_view bytes) {
        std::ofstream output(path, std::ios::binary);
        output << bytes;
    };
    try {
#ifdef _WIN32
        const auto program = root / "package/bin/claude.exe";
        write(root / "package/package.json", R"({"name":"@anthropic-ai/claude-code","bin":{"claude":"bin/claude.exe"}})");
        write(program, "MZfixture");
#else
        const auto program = root / "package/bin/claude";
        write(root / "package/package.json", R"({"name":"@anthropic-ai/claude-code","bin":{"claude":"bin/claude"}})");
#ifdef __APPLE__
        write(program, "\xcf\xfa\xed\xfe" "fixture");
#else
        write(program, "\x7f" "ELFfixture");
#endif
#endif
        const auto mcp = root / "empty.json";
        write(mcp, R"({"mcpServers":{}})");
        write(root / "profile/.credentials.json", addon::serialize_json(credentials(claude::now_ms() + 3600000)));
        const std::vector<std::string> arguments{"-p", "/usage", "--output-format", "json", "--verbose",
            "--no-session-persistence", "--safe-mode", "--strict-mcp-config", "--mcp-config", claude::path_string(mcp),
            "--tools", "", "--allowedTools", "", "--model", "sonnet"};
        const claude::Quota_environment environment{
            {"CLAUDE_CONFIG_DIR", claude::path_string(root / "profile")}, {"APPDATA", claude::path_string(root / "appdata")},
            {"ProgramFiles", claude::path_string(root / "program-files")}, {"HOME", claude::path_string(root)},
            {"XDG_CONFIG_HOME", claude::path_string(root / "appdata")}};
        const auto read = [&] {
            return claude::direct_quota_output(program, arguments, environment, root / "work",
                claude::Quota_clock::now() + std::chrono::seconds(30), fetch);
        };
#ifndef _WIN32
        utsname kernel{};
        require(uname(&kernel) == 0, "kernel policy context is observable");
        std::string release = kernel.release;
        for (auto& byte : release) if (byte >= 'A' && byte <= 'Z') byte += 'a' - 'A';
        if (release.find("microsoft") != std::string::npos) {
            require(!read() && requests == 0, "WSL retains native Windows-policy inheritance authority");
            fs::remove_all(root);
            return;
        }
#endif
        require(read().has_value() && requests == 1, "eligible complete path makes one request");
        write(root / "work/.git", "gitdir: /other-checkout/.git/worktrees/linked\n");
        require(!read() && requests == 1, "linked worktree's disjoint consent root remains native");
        fs::remove(root / "work/.git");
        fs::create_directories(root / "work/.claude");
        write(root / "work/.claude/settings.local.json", R"({"env":{"HTTPS_PROXY":"http://proxy.invalid"}})");
        require(!read() && requests == 1, "project network authority is inspected before sending credentials");
        fs::remove(root / "work/.claude/settings.local.json");
        write(root / "profile/.claude.json", R"({"cachedGrowthBookFeatures":{"tengu_windows_credman":true}})");
        require(!read() && requests == 1, "selected credential backend is inspected before sending credentials");
        fs::remove(root / "profile/.claude.json");
        write(root / "profile/remote-settings.json", "{}");
        require(!read() && requests == 1, "unresolved remote policy remains native");
        fs::remove(root / "profile/remote-settings.json");
        require(!claude::direct_quota_output(program, arguments, environment, root / "work",
            claude::Quota_clock::now(), fetch) && requests == 1, "expired operation cannot issue a request");
        reply = Reply::ERROR;
        require(!read() && requests == 2, "HTTP failure returns to the native route after one direct attempt");
        reply = Reply::ROTATED_CREDENTIAL;
        credential_path = root / "profile/.credentials.json";
        require(!read() && requests == 3, "rotated credential cannot publish an obsolete observation");
        write(credential_path, addon::serialize_json(credentials(claude::now_ms() + 3600000)));
        reply = Reply::LATE;
        require(!claude::direct_quota_output(program, arguments, environment, root / "work",
            claude::Quota_clock::now() + std::chrono::seconds(1), fetch) && requests == 4,
            "completion beyond the operation deadline cannot publish an observation");
        reply = Reply::USAGE;
        write(program, "#!/bin/sh\nexec other-claude \"$@\"\n");
        require(!read() && requests == 4, "an arbitrary selected wrapper is never bypassed");
    }
    catch (...) {
        fs::remove_all(root);
        throw;
    }
    fs::remove_all(root);
}

} // namespace

int main()
{
    try {
        admission_contract();
        response_contract();
        filesystem_contract();
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
