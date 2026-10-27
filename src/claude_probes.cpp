#include "claude_probes.h"
#include "claude_accounts.h"
#include "claude_launcher.h"
#include "claude_support.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <utility>

namespace claude {
namespace {

namespace addon = logonomic::cli_addon;
using Json = addon::Json_value;

struct Usage_result
{
    std::string content;
    bool local_usage_command = false;
};

std::optional<Usage_result> usage_result(const Json& event)
{
    if (text(event, "type") != "result") return std::nullopt;
    if (flag(event, "is_error") || text(event, "subtype") == "error") {
        throw addon::Quota_failure::REQUEST_FAILED;
    }
    return Usage_result{text(event, "result"), text(event, "local_command") == "usage"};
}

Usage_result usage_text(std::string_view bytes)
{
    addon::Json_parse_error error;
    const auto value = addon::parse_json(bytes, addon::Json_parse_limits{}, &error);
    if (value && value->is_object()) {
        if (auto result = usage_result(*value); result && !result->content.empty()) return *result;
    }
    if (value && value->is_array()) {
        for (const auto& event : value->as_array()) {
            const auto result = usage_result(event);
            if (result && !result->content.empty()) return *result;
        }
    }
    std::istringstream lines{std::string(bytes)};
    for (std::string line; std::getline(lines, line);) {
        const auto event = addon::parse_json(line, addon::Json_parse_limits{}, &error);
        if (!event || !event->is_object()) continue;
        const auto result = usage_result(*event);
        if (result && !result->content.empty()) return *result;
    }
    throw addon::Quota_failure::INVALID_RESPONSE;
}

std::optional<addon::Quota_window> quota_window(
    const std::string& content, const std::string& name, std::uint64_t minutes)
{
    const std::regex pattern("Current " + name + R"([^:\r\n]*:[ \t]*(\d+(?:\.\d+)?)% used([^\r\n]*))",
        std::regex::icase);
    std::smatch match;
    if (!std::regex_search(content, match, pattern)) return std::nullopt;
    const double used = std::stod(match[1].str());
    if (!std::isfinite(used)) {
        throw addon::Quota_failure::INVALID_RESPONSE;
    }
    addon::Quota_window window;
    window.used_percent   = used;
    window.window_minutes = minutes;
    window.label          = "Current " + name;
    // A missing native reset is unknown. Do not borrow the next window's reset.
    const auto tail = match[2].str();
    const std::regex reset_pattern(R"(resets[ \t]*([^\r\n]+))", std::regex::icase);
    if (!std::regex_search(tail, match, reset_pattern)) return window;
    auto reset = match[1].str();
    const auto trim_end = [&] {
        while (!reset.empty() && (reset.back() == ' ' || reset.back() == '\t')) reset.pop_back();
    };
    trim_end();
    while (!reset.empty()) {
        const bool punctuation = reset.back() == '.' || reset.back() == ',';
        const bool unmatched_close = reset.back() == ')' &&
            std::count(reset.begin(), reset.end(), ')') > std::count(reset.begin(), reset.end(), '(');
        if (!punctuation && !unmatched_close) {
            break;
        }
        reset.pop_back();
        trim_end();
    }
    const auto begin = reset.find_first_not_of(" \t");
    if (begin == std::string::npos) {
        throw addon::Quota_failure::INVALID_RESPONSE;
    }
    reset.erase(0, begin);
    window.reset_text = std::move(reset);
    return window;
}

addon::Observation_scope scope(const std::optional<addon::Account_context>& account)
{
    addon::Observation_scope result;
    if (account) result.source_account_key = account->adapter_account_key;
    result.observed_at_ms = now_ms();
    result.fresh_for_ms   = 60000;
    return result;
}

} // namespace

addon::Auth_status inspect_auth(const addon::Structured_inspection& inspection)
{
    if (inspection.timed_out) return addon::Auth_status::FAILED;
    addon::Json_parse_error error;
    const auto value = addon::parse_json(inspection.stdout_bytes, addon::Json_parse_limits{}, &error);
    const auto* logged_in = value && value->is_object() ? value->find("loggedIn") : nullptr;
    if (!logged_in || !logged_in->is_boolean()) return addon::Auth_status::UNKNOWN;
    if (logged_in->as_boolean() && inspection.exit_code == 0) return addon::Auth_status::AUTHENTICATED;
    if (!logged_in->as_boolean() && inspection.exit_code == 1) return addon::Auth_status::NOT_LOGGED_IN;
    return addon::Auth_status::UNKNOWN;
}

addon::Quota_inspect_result inspect_quota(const addon::Structured_inspection& inspection)
{
    if (inspection.timed_out) {
        return addon::make_quota_failure(addon::Quota_failure::TIMED_OUT);
    }
    if (inspection.exit_code != 0) {
        return addon::make_quota_failure(addon::Quota_failure::PROCESS_FAILED, {},
            addon::Quota_diagnostic{addon::Quota_failure_stage::PROCESS_EXIT, inspection.exit_code});
    }
    addon::Quota_observation observation;
    try {
        const auto usage = usage_text(inspection.stdout_bytes);
        observation.primary   = quota_window(usage.content, "session", 300);
        observation.secondary = quota_window(usage.content, "week", 10080);
        if (observation.primary && observation.secondary) {
            observation.state = addon::Quota_status::READY;
        }
        else
        if (observation.primary || observation.secondary || !usage.local_usage_command) {
            return addon::make_quota_failure(addon::Quota_failure::INVALID_RESPONSE);
        }
    }
    catch (addon::Quota_failure reason) {
        return addon::make_quota_failure(reason);
    }
    catch (const std::exception&) {
        return addon::make_quota_failure(addon::Quota_failure::INVALID_RESPONSE);
    }
    addon::Quota_inspect_result result;
    result.complete = true;
    result.observation = std::move(observation);
    return result;
}

void handle_probes(addon::Invocation_context& context, const std::filesystem::path& package_root)
{
    const bool authentication = context.request().capability.capability_id == "auth-status";
    addon::Contract_failure failure;
    if (context.request().operation == "prepare") {
        const auto input = addon::decode_probe_prepare_request(context.request().payload, &failure, 2);
        if (!input) {
            context.fail(addon::Result_status::FAILED, failure.reason_code);
            return;
        }
        addon::Structured_prepare_result result;
        result.process = cli_process(input->context, false);
        apply_account(input->context, result.process);
        auto& args = result.process.argv;
        if (authentication) {
            args.insert(args.end(), {"auth", "status", "--json"});
        }
        else {
            if (result.process.executable.kind == addon::Process_executable::Kind::PACKAGE_PATH) {
                args.insert(args.begin(), "--quota");
            }
            args.insert(args.end(), {"-p", "/usage", "--output-format", "json", "--verbose",
                "--no-session-persistence", "--safe-mode", "--strict-mcp-config", "--mcp-config",
                path_string(package_root / "assets/empty_mcp.json"), "--tools", "", "--allowedTools", "",
                "--model", "sonnet"});
            result.process.assets.push_back("assets/empty_mcp.json");
        }
        result.inspection.protocol_state = "await_exit";
        result.profile_storage = input->context.profile_storage;
        result.account_context = input->context.account_context;
        context.succeed(addon::encode_structured_prepare_result(result));
        return;
    }
    if (context.request().operation != "inspect") {
        context.reject_unknown_operation();
        return;
    }
    const auto input = addon::decode_probe_inspect_request(context.request().payload, &failure, 2);
    if (!input) {
        context.fail(addon::Result_status::FAILED, failure.reason_code);
        return;
    }
    if (authentication) {
        addon::Auth_inspect_result result;
        result.complete = input->inspection.final;
        if (result.complete) result.observation = addon::Auth_observation{scope(input->account_context), inspect_auth(input->inspection)};
        else result.protocol_state = input->inspection.protocol_state;
        context.succeed(addon::encode_auth_inspect_result(result));
    }
    else {
        addon::Quota_inspect_result result;
        result.complete = input->inspection.final;
        if (result.complete) {
            result = inspect_quota(input->inspection);
            result.observation->scope = scope(input->account_context);
        }
        else result.protocol_state = input->inspection.protocol_state;
        context.succeed(addon::encode_quota_inspect_result(result, 2));
    }
}

} // namespace claude
