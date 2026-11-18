#include "claude_accounts.h"
#include "claude_launcher.h"
#include "claude_filesystem.h"
#include "claude_native_context.h"
#include "claude_support.h"

#include <logonomic_cli_addon/launcher_utilities.h>
#include <logonomic_cli_addon/sha256.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace claude {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

fs::path normalized_profile(const fs::path& profile)
{
    if (profile.empty()) throw std::runtime_error("profile_unavailable");
    auto path = fs::absolute(profile).lexically_normal();
    if (path.has_relative_path() && !path.has_filename()) path = path.parent_path();
    const auto nfc = native_context::nfc_config_directory(path);
    std::error_code error;
    if (nfc.native() != path.native() && fs::equivalent(native_io_path(path), native_io_path(nfc), error)) return nfc;
    return path;
}

bool recorded_profile_matches(const fs::path& recorded, const fs::path& selected)
{
    if (!recorded.is_absolute()) return false;
    return addon::same_native_account_profile(normalized_profile(recorded), selected);
}

namespace {

fs::path selected_profile(const addon::Launch_environment& launch, std::string_view fallback, std::string_view normalization_directory)
{
    const auto environment = addon::environment_pairs(launch.effective_environment);
    auto profile = addon::environment_value(environment, "CLAUDE_CONFIG_DIR");
    if (profile.empty()) {
        auto home = addon::environment_value(
            environment, launch.platform_id == "windows-x86_64" ? "USERPROFILE" : "HOME");
        if (home.empty()) home = addon::environment_value(environment, "HOME");
        if (home.empty()) home = std::string(fallback);
        if (home.empty()) throw std::runtime_error("profile_unavailable");
        return normalized_profile(native_context::nfc_config_directory(addon::path_from_utf8(home) / ".claude"));
    }
    // The CLI normalizes the selector first. A relative selector subsequently
    // resolves below the original working directory, including its spelling.
    const auto path = native_context::nfc_config_directory(addon::path_from_utf8(profile));
    if (!path.is_absolute() && !addon::path_from_utf8(normalization_directory).is_absolute())
        throw std::runtime_error("profile_unavailable");
    return normalized_profile(path.is_absolute() ? path : addon::path_from_utf8(normalization_directory) / path);
}

addon::Account_record account(const fs::path& profile)
{
    addon::Account_record result;
    result.adapter_account_key = account_key(profile);
    result.display_name = addon::account_display_name(path_string(profile));
    result.profile_path = path_string(profile);
    // A profile that cannot be checked, such as one on an unreachable share, is
    // an unavailable account: it must not fail the discovery of every account.
    std::error_code error;
    result.status = fs::is_directory(profile, error)
        ? addon::Account_status::AVAILABLE
        : addon::Account_status::UNAVAILABLE;
    return result;
}

std::string key_for_identity(std::string_view identity)
{
    return "profile-" + addon::to_hex(addon::Sha256::digest(identity));
}

fs::path granted_profile(const addon::Account_context& account, const addon::Session_launch_context* launch)
{
    if (!account.profile_path) throw std::runtime_error("account_unavailable");
    const auto retained = addon::path_from_utf8(*account.profile_path).lexically_normal();
    const auto profile  = normalized_profile(retained);
    if (account_key(profile) == account.adapter_account_key) return profile;

    // Older keys folded native Windows names together and included trailing
    // separators. Keep the granted spelling, and verify any independent
    // launcher selection before accepting the original key.
    if (launch && launch->launch_environment) {
        const auto selected = selected_profile(*launch->launch_environment, {}, launch->working_directory);
        if (!addon::same_native_account_profile(profile, selected)) throw std::runtime_error("account_profile_mismatch");
    }
    else
    if (profile.native() != native_context::nfc_config_directory(profile).native()) {
        // A grant without independent launcher evidence cannot distinguish a
        // former absolute selector from a relative one resolved under this cwd.
        throw std::runtime_error("account_profile_mismatch");
    }
    const auto historical = addon::historical_account_profile_identity(retained, retained);
    if (historical && key_for_identity(*historical) == account.adapter_account_key) return profile;
    throw std::runtime_error("account_profile_mismatch");
}

} // namespace

std::string account_key(const fs::path& profile)
{
    const auto location = normalized_profile(profile);
    auto identity = addon::native_account_profile_identity(location);
    if (!identity) throw std::runtime_error("profile_normalization_failed");
    // An old absolute selector used the same pre-NFC key for a different
    // native location. 0xff cannot begin a UTF-8 path or an SDK path marker.
    if (location.native() != native_context::nfc_config_directory(location).native()) identity->insert(0, 1, (char)0xff);
    return key_for_identity(*identity);
}

fs::path profile_for_account(const addon::Account_context& account)
{
    return granted_profile(account, nullptr);
}

fs::path profile_for_context(const addon::Session_launch_context& context)
{
    if (context.account_context) return granted_profile(*context.account_context, &context);
    if (!context.launch_environment) throw std::runtime_error("launch_environment_unavailable");
    return selected_profile(*context.launch_environment, {}, context.working_directory);
}

std::string profile_selector_for_launch(const addon::Session_launch_context& context)
{
    const auto profile = profile_for_context(context);
    if (profile.native() == native_context::nfc_config_directory(profile).native()) return path_string(profile);
    const auto cwd = addon::path_from_utf8(context.working_directory);
    if (cwd.is_absolute()) {
        const auto relative = profile.lexically_relative(cwd.lexically_normal());
        if (!relative.empty() && recorded_profile_matches(cwd / native_context::nfc_config_directory(relative), profile)) {
            return path_string(relative);
        }
    }
    throw std::runtime_error("account_profile_unrepresentable");
}

void apply_account(const addon::Session_launch_context& context, addon::Process_proposal& process)
{
    process.environment_names = {"PATH", "PATHEXT", "HOME", "USERPROFILE", "LOCALAPPDATA", "APPDATA",
        "XDG_CONFIG_HOME", "XDG_DATA_HOME", "SystemRoot", "WINDIR", "TEMP", "TMP"};
    process.environment.emplace_back("CLAUDE_CONFIG_DIR", profile_selector_for_launch(context));
}

void handle_accounts(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    const auto input = addon::decode_accounts_launcher_request(context.request().payload, &failure);
    if (!input) {
        context.fail(addon::Result_status::FAILED, failure.reason_code);
        return;
    }
    if (!recognize_launcher(input->launcher.script_body, input->launcher.runner_key)) {
        context.succeed(context.request().operation == "discover" ? addon::encode_accounts({})
            : addon::encode_account_resolution({std::nullopt}));
        return;
    }
    const auto selected = selected_profile(input->launch_environment, input->default_home_fallback, input->normalization_directory);
    if (context.request().operation == "resolve") {
        context.succeed(addon::encode_account_resolution({account_key(selected)}));
        return;
    }
    if (context.request().operation != "discover") {
        context.reject_unknown_operation();
        return;
    }
    std::vector<addon::Account_record> accounts{account(selected)};
    const auto parent = selected.parent_path();
    std::error_code error;
    if (fs::is_directory(parent, error)) {
        std::vector<fs::path> candidates;
        for (fs::directory_iterator entry(parent, error), end; !error && entry != end; entry.increment(error)) {
            if (context.cancel_requested()) {
                context.fail(addon::Result_status::CANCELLED, "cancelled");
                return;
            }
            std::error_code entry_error;
            const auto name = path_string(entry->path().filename());
            if (!name.starts_with(".claude") || !addon::is_valid_utf8(name) || !entry->is_directory(entry_error)) continue;
            const auto profile = normalized_profile(parent / native_context::nfc_config_directory(entry->path().filename()));
            if (fs::is_regular_file(profile / ".credentials.json", entry_error)) candidates.push_back(profile);
        }
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
        for (const auto& candidate : candidates) {
            if (account_key(candidate) == accounts.front().adapter_account_key) continue;
            if (accounts.size() == 64) throw std::runtime_error("account_inventory_capacity");
            accounts.push_back(account(candidate));
        }
    }
    context.succeed(addon::encode_accounts(accounts));
}

} // namespace claude
