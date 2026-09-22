#include "claude_accounts.h"
#include "claude_launcher.h"
#include "claude_lifecycle.h"
#include "claude_support.h"

#include <logonomic_cli_addon/launcher_utilities.h>
#include <logonomic_cli_addon/legacy_state_import.h>
#include <logonomic_cli_addon/sha256.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace claude {
namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
using Json = addon::Json_value;

fs::path normalize(const fs::path& profile)
{
    if (profile.empty()) throw std::runtime_error("profile_unavailable");
    return fs::absolute(profile).lexically_normal();
}

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
        return normalize(addon::path_from_utf8(home) / ".claude");
    }
    const auto path = addon::path_from_utf8(profile);
    return normalize(path.is_absolute() ? path : addon::path_from_utf8(normalization_directory) / path);
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

} // namespace

std::string account_key(const fs::path& profile)
{
    auto normalized = normalize(profile);
#ifdef _WIN32
    const auto source = normalized.native();
    const int count = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, source.data(),
        static_cast<int>(source.size()), nullptr, 0, nullptr, nullptr, 0);
    if (count <= 0) throw std::runtime_error("profile_normalization_failed");
    std::wstring folded(static_cast<std::size_t>(count), L'\0');
    if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, source.data(), static_cast<int>(source.size()),
            folded.data(), count, nullptr, nullptr, 0) != count)
    {
        throw std::runtime_error("profile_normalization_failed");
    }
    normalized = fs::path(folded);
#endif
    return "profile-" + addon::to_hex(addon::Sha256::digest(path_string(normalized)));
}

fs::path profile_for_context(const addon::Session_launch_context& context)
{
    if (context.account_context) {
        if (!context.account_context->profile_path) throw std::runtime_error("account_unavailable");
        const auto profile = normalize(addon::path_from_utf8(*context.account_context->profile_path));
        if (account_key(profile) != context.account_context->adapter_account_key) throw std::runtime_error("account_profile_mismatch");
        return profile;
    }
    if (!context.launch_environment) throw std::runtime_error("launch_environment_unavailable");
    return selected_profile(*context.launch_environment, {}, context.working_directory);
}

void apply_account(const addon::Session_launch_context& context, addon::Process_proposal& process)
{
    process.environment_names = {"PATH", "PATHEXT", "HOME", "USERPROFILE", "LOCALAPPDATA", "APPDATA",
        "XDG_CONFIG_HOME", "XDG_DATA_HOME", "SystemRoot", "WINDIR", "TEMP", "TMP"};
    process.environment.emplace_back("CLAUDE_CONFIG_DIR", path_string(profile_for_context(context)));
}

void handle_accounts(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    const auto input = addon::decode_accounts_launcher_request(context.request().payload, &failure);
    if (!input) {
        context.fail(addon::Result_status::FAILED, failure.reason_code);
        return;
    }
    if (!recognize_launcher(input->launcher.script_body)) {
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
            if (entry->is_directory(entry_error) && path_string(entry->path().filename()).starts_with(".claude") &&
                fs::is_regular_file(entry->path() / ".credentials.json", entry_error))
            {
                candidates.push_back(normalize(entry->path()));
            }
        }
        std::sort(candidates.begin(), candidates.end());
        for (const auto& candidate : candidates) {
            if (account_key(candidate) == account_key(selected)) continue;
            if (accounts.size() == 64) throw std::runtime_error("account_inventory_capacity");
            accounts.push_back(account(candidate));
        }
    }
    context.succeed(addon::encode_accounts(accounts));
}

void handle_history(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    const auto input = addon::decode_import_record_request(context.request().payload, &failure);
    if (!input) {
        context.fail(addon::Result_status::FAILED, failure.reason_code);
        return;
    }
    addon::Legacy_import_result result;
    if (input->kind == addon::k_legacy_resume_record_kind) {
        const auto record = addon::decode_legacy_resume_record(input->record, &failure);
        if (!record) {
            context.fail(addon::Result_status::FAILED, failure.reason_code);
            return;
        }
        if (record->provider_id == "claude" && canonical_session_id(record->provider_session_id)) {
            Json converted = Json::object();
            converted.set("provider_session_id", Json(record->provider_session_id));
            result = *addon::Legacy_import_result::from_conversion(addon::encode_legacy_resume_conversion(converted));
        }
    }
    else
    if (input->kind == addon::k_legacy_account_record_kind) {
        const auto record = addon::decode_legacy_account_record(input->record, &failure);
        if (!record) {
            context.fail(addon::Result_status::FAILED, failure.reason_code);
            return;
        }
        if (record->provider_id == "claude" && record->profile_path) {
            const auto profile = normalize(addon::path_from_utf8(*record->profile_path));
            Json converted = Json::object();
            converted.set("adapter_account_key", Json(account_key(profile)));
            result = *addon::Legacy_import_result::from_conversion(std::move(converted));
        }
    }
    else
    if (input->kind == addon::k_legacy_usage_records_kind) {
        const auto record = addon::decode_legacy_usage_records(input->record, &failure);
        if (!record) {
            context.fail(addon::Result_status::FAILED, failure.reason_code);
            return;
        }
        if (record->provider_id == "claude") {
            // Claude's native store is keyed by exact provider Session id. The
            // scanner retains that aggregation boundary and imports no counters.
            auto converted = record->records;
            for (auto& identity : converted) {
                if (identity.session_key) identity.record_id = *identity.session_key;
                if (identity.record_id.empty()) throw std::runtime_error("invalid_usage_identity");
            }
            result = *addon::Legacy_import_result::from_conversion(addon::encode_legacy_usage_conversion(converted));
        }
    }
    else {
        context.fail(addon::Result_status::UNAVAILABLE, "unknown_record_kind");
        return;
    }
    context.succeed(addon::encode_import_record_result(result));
}

} // namespace claude
