#include "claude_lifecycle.h"
#include "claude_support.h"

#include <logonomic_cli_addon/capability_ids.h>
#include <logonomic_cli_addon/context_state.h>
#include <logonomic_cli_addon/execution_context.h>
#include <logonomic_cli_addon/limits.h>
#include <logonomic_cli_addon/provider_event_diagnostic.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace claude {
namespace {

namespace addon = logonomic::cli_addon;

std::string metadata(const addon::Provider_event& event, std::string_view field)
{
    const auto* member = event.body.find(field);
    const auto reject = [&](addon::Provider_event_problem problem, std::optional<std::uint64_t> limit = {}) {
        return addon::Provider_event_error(addon::make_provider_event_diagnostic(
            event.name, std::string(field), problem, member, limit, "nonempty_single_line_identity"));
    };
    if (!member) {
        throw reject(addon::Provider_event_problem::MISSING);
    }
    if (!member->is_string()) {
        throw reject(addon::Provider_event_problem::WRONG_TYPE);
    }
    auto value = member->as_string();
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        throw reject(addon::Provider_event_problem::EMPTY);
    }
    value = value.substr(begin, value.find_last_not_of(" \t\r\n") - begin + 1);
    if (value.size() > addon::limits::k_max_lifecycle_identity_bytes) {
        auto diagnostic = addon::make_provider_event_diagnostic(
            event.name, std::string(field), addon::Provider_event_problem::TOO_LARGE, member,
            addon::limits::k_max_lifecycle_identity_bytes, "nonempty_single_line_identity");
        diagnostic.actual_size = value.size();
        throw addon::Provider_event_error(std::move(diagnostic));
    }
    if (value.find_first_of(std::string_view("\0\r\n", 3)) != std::string::npos) {
        throw reject(addon::Provider_event_problem::CONTROL_CHARACTER);
    }
    return value;
}

void classify_context(
    const addon::Classify_event_request& request, addon::Classify_event_result& result)
{
    const auto& body = request.event.body;
    if (request.context_state_version != addon::k_context_state_capability_version || body.find("agent_id")) {
        return;
    }
    const bool compact = request.event.name == "SessionStart" && text(body, "source") == "compact";
    const auto* context = body.find("context_state");
    if (!compact &&
        (!context || (request.event.name != "ModContext" && request.event.name != "ModTurnComplete")))
    {
        return;
    }
    auto update = compact || !context->is_object() ? addon::Json_value::object() : *context;
    if (compact) {
        // Native compaction hooks precede installation of the replacement
        // boundary. Invalidate usage without claiming an installed compaction.
        update.set("usage_unavailable", addon::Json_value(true));
    }
    else
    if (!context->is_object()) {
        // Keep malformed telemetry attributable to its native source without
        // carrying arbitrary payload content into a failure diagnostic.
        update.set("usage_unavailable", addon::Json_value());
    }
    const auto* session = body.find("session_id");
    update.set("provider_session_id", session ? *session : addon::Json_value());
    update.set("event_id", addon::Json_value(request.transition_nonce));
    auto updates = addon::Json_value::array();
    updates.push_back(std::move(update));
    auto payload = addon::Json_value::object();
    payload.set("context_updates", std::move(updates));
    addon::decode_context_updates(payload, result.context_updates, result.context_failure);
}

void classify_start_directory(
    const addon::Classify_event_request& request, addon::Classify_event_result& result,
    const std::string& provider_session_id)
{
    if (request.execution_context_version != addon::k_execution_context_version || request.execution_failure) {
        return;
    }
    addon::Execution_context_update update;
    update.provider_session_id = provider_session_id;
    update.transition_nonce    = request.transition_nonce;
    update.source              = "claude.SessionStart";
    const auto* cwd = request.event.body.find("cwd");
    if (cwd && cwd->is_string() && addon::is_execution_local_path(cwd->as_string())) {
        update.status      = addon::Execution_context_status::CONFIRMED;
        update.environment = "local";
        update.cwd         = cwd->as_string();
    }
    // Missing or unsupported cwd is an independent unknown fact. The native
    // identity still transitions, and no launch or Git root becomes a fallback.
    result.execution_updates.push_back(std::move(update));
}

} // namespace

bool canonical_session_id(std::string_view value)
{
    if (value.size() != 36) {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (value[index] != '-') return false;
        }
        else
        if (!((value[index] >= '0' && value[index] <= '9') || (value[index] >= 'a' && value[index] <= 'f'))) {
            return false;
        }
    }
    return true;
}

addon::Classify_event_result classify_event(const addon::Classify_event_request& request)
{
    const auto& body = request.event.body;
    const auto name = metadata(request.event, "hook_event_name");
    if (name != request.event.name) {
        throw addon::Provider_event_error(addon::make_provider_event_diagnostic(
            request.event.name, "hook_event_name", addon::Provider_event_problem::MISMATCH,
            body.find("hook_event_name"), {}, "matches_event_name"));
    }
    addon::Classify_event_result result;
    if (request.context_failure) {
        result.context_failure = addon::Context_failure{{}, *request.context_failure};
    }
    result.execution_failure = request.execution_failure;
    const std::array<std::string_view, 13> supported{
        "SessionStart", "SessionEnd", "SubagentStart", "SubagentStop", "UserPromptSubmit", "Stop",
        "StopFailure", "PermissionRequest", "PreToolUse", "PostToolUse", "PostToolUseFailure", "ModTurnComplete",
        "ModContext",
    };
    if (std::find(supported.begin(), supported.end(), name) == supported.end()) {
        return result;
    }
    const bool child = name == "SubagentStart" || name == "SubagentStop";
    const bool tool = name == "PreToolUse" || name == "PostToolUse" || name == "PostToolUseFailure";
    if ((!child && body.find("agent_id")) ||
        (tool && text(body, "tool_name") != "AskUserQuestion" && text(body, "tool_name") != "ExitPlanMode"))
    {
        return result;
    }
    classify_context(request, result);
    if (name == "ModContext" || (name == "ModTurnComplete" && text(body, "reason") != "answer") ||
        (name == "SessionStart" && text(body, "source") == "compact"))
    {
        return result;
    }
    addon::Lifecycle_observation observation;
    observation.provider_event_name = name;
    observation.provider_session_id = metadata(request.event, "session_id");
    result.classification = addon::Classification::EVENT;
    if (name == "SessionStart") {
        observation.session_source = metadata(request.event, "source");
        observation.session_transition = addon::Session_transition::STARTED;
        result.classification = addon::Classification::TRANSITION;
        result.transition = observation.session_source == "resume" ? addon::Transition_kind::RESUME : addon::Transition_kind::START;
        if (canonical_session_id(observation.provider_session_id)) {
            auto record = addon::Json_value::object();
            record.set("provider_session_id", addon::Json_value(observation.provider_session_id));
            observation.resume_record = std::move(record);
        }
        classify_start_directory(request, result, observation.provider_session_id);
    }
    else
    if (name == "SessionEnd") {
        observation.session_transition = addon::Session_transition::ENDED;
        observation.session_end_reason = metadata(request.event, "reason");
    }
    else
    if (child) {
        observation.agent_id = metadata(request.event, "agent_id");
        observation.agent_label = metadata(request.event, "agent_type");
        observation.subagent_transition = name == "SubagentStart" ? addon::Subagent_transition::STARTED : addon::Subagent_transition::STOPPED;
    }
    else {
        if (name == "ModTurnComplete") {
            observation.phase_transition = addon::Phase_transition::AWAITING_INPUT;
            observation.phase_evidence = addon::Evidence::CONFIRMED;
            result.observations.push_back(std::move(observation));
            return result;
        }
        if (body.find("prompt_id")) observation.provider_turn_id = metadata(request.event, "prompt_id");
        if (name == "UserPromptSubmit" || name == "Stop" || name == "StopFailure") {
            observation.phase_transition = name == "UserPromptSubmit" ? addon::Phase_transition::WORKING : addon::Phase_transition::AWAITING_INPUT;
            observation.phase_evidence = addon::Evidence::INFERRED;
            if (name == "StopFailure" && !observation.provider_turn_id.empty()) {
                (void)metadata(request.event, "error");
                observation.outcome_transition = addon::Outcome_transition::FAILED;
                observation.outcome_evidence = addon::Evidence::CONFIRMED;
            }
        }
        else {
            observation.attention_transition = name == "PostToolUse" || name == "PostToolUseFailure"
                ? addon::Attention_transition::RESOLVED : addon::Attention_transition::REQUESTED;
            observation.attention_evidence = addon::Evidence::INFERRED;
            if (tool) observation.attention_request_id = metadata(request.event, "tool_use_id");
        }
    }
    result.observations.push_back(std::move(observation));
    return result;
}

addon::Attention_classification_result classify_attention(const addon::Attention_classification_request& request)
{
    addon::Attention_classification_result result{classify_event(request.base), std::nullopt};
    if (result.base.observations.empty()) return result;
    auto& observation = result.base.observations.front();
    const auto& name = request.base.event.name;
    if (name == "Stop") {
        const auto* evidence = request.base.event.body.find("assistant_reply_evidence");
        if (evidence && addon::looks_like_reply_request_evidence(*evidence)) {
            observation.attention_transition = addon::Attention_transition::REQUESTED;
            observation.attention_evidence = addon::Evidence::INFERRED;
            observation.attention_request_id = "conversation-" + std::to_string(request.base.ledger.current_epoch) + "-" +
                (request.base.ledger.current_transition ? request.base.ledger.current_transition->nonce : std::string{}) + "-" +
                addon::provider_event_evidence_digest_hex(request.base.event);
            result.attention_kind = addon::Attention_kind{0, "conversational_hint"};
        }
    }
    else if (name == "UserPromptSubmit" && request.pending_conversational_request_id) {
        observation.attention_transition = addon::Attention_transition::RESOLVED;
        observation.attention_evidence = addon::Evidence::INFERRED;
        observation.attention_request_id = *request.pending_conversational_request_id;
        result.attention_kind = addon::Attention_kind{0, "conversational_hint"};
    }
    else if (observation.attention_transition != addon::Attention_transition::NONE) {
        const bool permission = name == "PermissionRequest" ||
            text(request.base.event.body, "tool_name") == "ExitPlanMode";
        result.attention_kind = addon::Attention_kind{0,
            permission ? "permission" : "interactive_question"};
    }
    return result;
}

void handle_lifecycle(addon::Invocation_context& context)
{
    if (context.request().operation != addon::k_classify_event_operation) {
        context.reject_unknown_operation();
        return;
    }
    addon::Contract_failure failure;
    if (context.request().capability.capability_id == addon::k_attention_kind_capability_id) {
        const auto request = addon::decode_attention_classification_request(context.request().payload, &failure);
        if (!request) {
            context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        }
        else {
            context.succeed(addon::encode_attention_classification_result(classify_attention(*request)));
        }
        return;
    }
    const auto request = addon::decode_classify_event_request(context.request().payload, &failure);
    if (!request) {
        context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }
    context.succeed(addon::encode_classify_event_result(classify_event(*request)));
}

} // namespace claude
