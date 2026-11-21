#include "claude_accounts.h"
#include "claude_conversation.h"
#include "claude_lifecycle.h"
#include "claude_launcher.h"
#include "claude_launcher_approval.h"
#include "claude_preparation.h"
#include "claude_probes.h"
#include "claude_sessions.h"
#include "claude_usage.h"
#include "claude_usage_summary.h"
#include "claude_wake.h"

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/capability_ids.h>
#include <logonomic_cli_addon/profile_storage.h>
#include <logonomic_cli_addon/provider_event_diagnostic.h>
#include <logonomic_cli_addon/package_source.h>

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

#include <exception>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

fs::path executable_path()
{
#ifdef _WIN32
    std::vector<wchar_t> buffer(32768);
    const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (size == 0 || size >= buffer.size()) throw std::runtime_error("executable_path_unavailable");
    return fs::path(std::wstring(buffer.data(), size));
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    if (size == 0 || size > 65536) throw std::runtime_error("executable_path_unavailable");
    std::vector<char> buffer(size);
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) throw std::runtime_error("executable_path_unavailable");
    return fs::canonical(addon::path_from_utf8(buffer.data()));
#else
    std::vector<char> buffer(65536);
    const auto size = readlink("/proc/self/exe", buffer.data(), buffer.size());
    if (size < 0 || static_cast<std::size_t>(size) >= buffer.size()) throw std::runtime_error("executable_path_unavailable");
    return addon::path_from_utf8(std::string_view(buffer.data(), static_cast<std::size_t>(size)));
#endif
}

void invoke(addon::Invocation_context& context, const fs::path& package_root)
{
    try {
        if (context.cancel_requested()) {
            context.fail(addon::Result_status::CANCELLED, "cancelled");
            return;
        }
        const auto& request = context.request();
        const auto& capability = request.capability.capability_id;
        if (capability == addon::k_launcher_approval_capability_id) {
            addon::handle_launcher_approval(context, claude::launcher_approval_rules());
        }
        else
        if (capability == "launcher-discovery") {
            addon::Contract_failure failure;
            if (request.operation == "discover") {
                const auto input = addon::decode_discover_request(request.payload, &failure);
                if (!input) context.fail(addon::Result_status::FAILED, failure.reason_code);
                else context.succeed(addon::encode_candidates(claude::discover_launchers(*input)));
            }
            else
            if (request.operation == "recognize") {
                const auto input = addon::decode_recognize_request(request.payload, &failure);
                if (!input) context.fail(addon::Result_status::FAILED, failure.reason_code);
                else {
                    addon::Launcher_recognition result;
                    result.recognized = claude::recognize_launcher(input->launcher.script_body, input->launcher.runner_key).has_value();
                    if (result.recognized) result.display_name = "Claude";
                    context.succeed(addon::encode_recognition(result));
                }
            }
            else context.reject_unknown_operation();
        }
        else
        if (capability == "profile-storage" && request.operation == "acknowledge") {
            addon::acknowledge_profile_storage(context);
        }
        else
        if (capability == "accounts") claude::handle_accounts(context);
        else
        if (capability == addon::k_conversation_export_capability_id) {
            addon::handle_conversation_export(context, claude::export_conversation);
        }
        else
        if (capability == addon::k_session_wake_capability_id) claude::handle_wake(context);
        else
        if (capability == "usage-scan" || capability == "usage-incremental") claude::handle_usage(context);
        else
        if (capability == "usage-lifetime-summary") claude::handle_usage_summary(context);
        else
        if (capability == "usage-saved-daily-tokens") claude::handle_usage_daily(context);
        else
        if (capability == "auth-status" || capability == "quota") claude::handle_probes(context, package_root);
        else
        if (capability == "structured-session" || capability == "delegated-session") claude::handle_sessions(context, package_root);
        else
        if (capability == "helper-ingress" || capability == "lifecycle-events" ||
            capability == addon::k_attention_kind_capability_id) claude::handle_lifecycle(context);
        else claude::handle_preparation(context, package_root);
    }
    catch (const addon::Provider_event_error& error) {
        const bool cancelled = context.cancel_requested();
        context.fail(cancelled ? addon::Result_status::CANCELLED : addon::Result_status::FAILED,
            cancelled ? "cancelled" : "invalid_provider_event",
            cancelled ? std::string{} : addon::encode_provider_event_diagnostic(error.diagnostic()));
    }
    catch (const std::exception&) {
        // Provider config, credentials and filesystem paths stay out of diagnostics.
        const bool cancelled = context.cancel_requested();
        context.fail(cancelled ? addon::Result_status::CANCELLED : addon::Result_status::FAILED,
            cancelled ? "cancelled" : "claude_operation_failed");
    }
}

} // namespace

int main()
{
    const fs::path package_root = executable_path().parent_path().parent_path();
    addon::Adapter_runtime_options options;
    options.adapter_name = "claude-addon";
    options.adapter_version = LOGONOMIC_CLI_ADDON_ADAPTER_VERSION;
    options.capabilities = {
        {addon::k_launcher_approval_capability_id, addon::k_launcher_approval_version},
        {"presentation", 1}, {"launcher-discovery", 1}, {"profile-storage", 1}, {"managed-assets", 1},
        {addon::k_conversation_export_capability_id, addon::k_conversation_export_version},
        {"accounts", 2}, {"launch-preparation", 3}, {"interactive-resume", 2}, {"interactive-resume", 3},
        {"interactive-resume", addon::k_interactive_resume_content_version},
        {"structured-session", 3}, {"delegated-session", 3}, {"auth-status", 2}, {"quota", 3},
        {addon::k_delegated_model_selection_capability_id, addon::k_delegated_model_selection_version},
        {"usage-scan", 3}, {"lifecycle-events", 2}, {"helper-ingress", 2},
        {"usage-lifetime-summary", 1},
        {"usage-saved-daily-tokens", 1},
        {"usage-incremental", 1},
        {addon::k_context_state_capability_id, addon::k_context_state_capability_version},
        {addon::k_model_state_capability_id, addon::k_model_state_version},
        {addon::k_structured_model_observation_capability_id, addon::k_structured_model_observation_version},
        {addon::k_execution_context_capability_id, addon::k_execution_context_version},
        {addon::k_usage_temporal_capability_id, addon::k_usage_temporal_version},
        {addon::k_attention_kind_capability_id, addon::k_attention_kind_version},
        {"coordination-delivery", 1},
        {addon::k_interactive_initial_prompt_capability_id, addon::k_interactive_initial_prompt_version},
        {addon::k_session_wake_capability_id, addon::k_session_wake_version},
        {addon::k_model_context_delivery_capability_id, addon::k_model_context_delivery_version},
        {addon::k_quota_failure_capability_id, addon::k_quota_failure_version},
        {addon::k_quota_cost_capability_id, addon::k_quota_cost_version},
    };
    addon::Adapter_runtime runtime(std::move(options));
    runtime.set_invoke_handler([package_root](addon::Invocation_context& context) { invoke(context, package_root); });
    return runtime.run();
}
