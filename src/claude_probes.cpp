#include "claude_probes.h"
#include "claude_accounts.h"
#include "claude_cost.h"
#include "claude_launcher.h"
#include "claude_quota_windows.h"
#include "claude_support.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace claude {
namespace {

namespace addon = logonomic::cli_addon;
using Json = addon::Json_value;

struct Usage_result
{
    std::string content;
    bool local_usage_command = false;
    std::optional<Json> limits;
};

std::optional<Usage_result> usage_result(const Json& event)
{
    if (text(event, "type") == "result" && (flag(event, "is_error") || text(event, "subtype") == "error")) {
        throw addon::Quota_failure::REQUEST_FAILED;
    }
    const auto* report = event.find("usage_report");
    if (report && report->is_object()) {
        const auto* rates = report->find("rate_limits");
        const auto* limits = rates ? rates->find("limits") : nullptr;
        if (limits && !limits->is_null()) {
            if (!limits->is_array()) {
                throw addon::Quota_failure::INVALID_RESPONSE;
            }
            return Usage_result{{}, true, *limits};
        }
    }
    const auto* run = event.find("local_command_run");
    const bool native_usage_command = text(event, "type") == "assistant" && run &&
        text(*run, "command") == "usage" && !run->find("plugin");
    if ((report && report->is_object()) || native_usage_command) {
        const auto* message = event.find("message");
        const auto* content = message ? message->find("content") : nullptr;
        if (content && content->is_array()) {
            std::string fallback;
            for (const auto& block : content->as_array()) {
                if (text(block, "type") == "text") {
                    if (!fallback.empty()) {
                        fallback += '\n';
                    }
                    fallback += text(block, "text");
                }
            }
            if (!fallback.empty()) {
                return Usage_result{std::move(fallback), true, std::nullopt};
            }
        }
    }
    if (text(event, "type") != "result") {
        return std::nullopt;
    }
    return Usage_result{text(event, "result"), text(event, "local_command") == "usage", std::nullopt};
}

Usage_result usage_text(std::string_view bytes)
{
    addon::Json_parse_error error;
    const auto value = addon::parse_json(bytes, addon::Json_parse_limits{}, &error);
    std::optional<Usage_result> structured;
    std::optional<Usage_result> fallback;
    const auto accept = [&](const Json& event) {
        auto result = usage_result(event);
        if (result && result->limits) {
            structured = std::move(result);
        }
        else
        if (result && !result->content.empty()) {
            fallback = std::move(result);
        }
    };
    if (value && value->is_object()) {
        accept(*value);
    }
    else
    if (value && value->is_array()) {
        for (const auto& event : value->as_array()) {
            accept(event);
        }
    }
    else {
        std::istringstream lines{std::string(bytes)};
        for (std::string line; std::getline(lines, line);) {
            const auto event = addon::parse_json(line, addon::Json_parse_limits{}, &error);
            if (event && event->is_object()) {
                accept(*event);
            }
        }
    }
    if (structured) {
        return std::move(*structured);
    }
    if (fallback) {
        return std::move(*fallback);
    }
    throw addon::Quota_failure::INVALID_RESPONSE;
}

addon::Quota_window quota_window(const std::smatch& row)
{
    const double used = std::stod(row[2].str());
    if (!std::isfinite(used) || used < 0) {
        throw addon::Quota_failure::INVALID_RESPONSE;
    }
    addon::Quota_window window;
    window.used_percent = used;
    auto label = row[1].str();
    label.erase(0, label.find_first_not_of(" \t"));
    label.erase(label.find_last_not_of(" \t") + 1);
    window.label = std::move(label);
    // A missing native reset is unknown. Do not borrow the next window's reset.
    const auto tail = row[3].str();
    const std::regex reset_pattern(R"(resets[ \t]*([^\r\n]+))", std::regex::icase);
    std::smatch match;
    if (!std::regex_search(tail, match, reset_pattern)) {
        return window;
    }
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
        if (usage.limits) {
            observation.windows = parse_quota_windows(*usage.limits);
            if (!observation.windows) {
                return addon::make_quota_failure(addon::Quota_failure::INVALID_RESPONSE);
            }
            observation.state = addon::Quota_status::READY;
        }
        else {
            std::vector<addon::Quota_window> windows;
            const std::regex pattern(R"(^[ \t]*([^\r\n]+):[ \t]*(-?\d+(?:\.\d+)?)% used([^\r\n]*)$)",
                std::regex::icase);
            std::istringstream lines(usage.content);
            for (std::string line; std::getline(lines, line);) {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                std::smatch row;
                if (std::regex_match(line, row, pattern)) {
                    windows.push_back(quota_window(row));
                }
                else
                if (line.find("% used") != std::string::npos) {
                    return addon::make_quota_failure(addon::Quota_failure::INVALID_RESPONSE);
                }
            }
            if (!windows.empty()) {
                observation.state = addon::Quota_status::READY;
                observation.windows = std::move(windows);
            }
            else
            if (!usage.local_usage_command) {
                return addon::make_quota_failure(addon::Quota_failure::INVALID_RESPONSE);
            }
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
    if (!addon::decode_quota_inspect_result(addon::encode_quota_inspect_result(result, 3), true, nullptr, 3)) {
        return addon::make_quota_failure(addon::Quota_failure::INVALID_RESPONSE);
    }
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
            if (input->report_quota_cost && input->account_context &&
                (!result.observation->windows || result.observation->windows->empty()))
            {
                // Saved costs remain account evidence even when the current
                // native quota probe failed. Its original status is unchanged.
                try {
                    result.cost = read_recorded_cost(*input->account_context, context);
                }
                catch (const std::exception&) {
                    // Optional local evidence cannot cancel or replace the
                    // completed core quota result, including on cancellation.
                    result.cost_failed = true;
                }
            }
        }
        else result.protocol_state = input->inspection.protocol_state;
        context.succeed(addon::encode_quota_inspect_result(
            result, 3, input->report_quota_failure, input->report_quota_cost));
    }
}

} // namespace claude
