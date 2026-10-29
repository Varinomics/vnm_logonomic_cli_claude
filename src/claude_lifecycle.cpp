#include "claude_lifecycle.h"
#include "claude_support.h"

#include <logonomic_cli_addon/capability_ids.h>
#include <logonomic_cli_addon/context_state.h>
#include <logonomic_cli_addon/limits.h>

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace claude {
namespace {

namespace addon = logonomic::cli_addon;

std::string metadata(const addon::Json_value& body, std::string_view field)
{
    auto value = text(body, field);
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        throw std::runtime_error("invalid_provider_event");
    }
    value = value.substr(begin, value.find_last_not_of(" \t\r\n") - begin + 1);
    if (value.size() > addon::limits::k_max_lifecycle_identity_bytes ||
        value.find_first_of(std::string_view("\0\r\n", 3)) != std::string::npos)
    {
        throw std::runtime_error("invalid_provider_event");
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
    const auto name = metadata(body, "hook_event_name");
    if (name != request.event.name) {
        throw std::runtime_error("invalid_provider_event");
    }
    addon::Classify_event_result result;
    if (request.context_failure) {
        result.context_failure = addon::Context_failure{{}, *request.context_failure};
    }
    const std::array<std::string_view, 13> supported{
        "SessionStart", "SessionEnd", "SubagentStart", "SubagentStop", "UserPromptSubmit", "Stop",
        "StopFailure", "PermissionRequest", "PreToolUse", "PostToolUse", "PostToolUseFailure", "ModTurnComplete",
        "ModContext",
    };
    if (std::find(supported.begin(), supported.end(), name) == supported.end()) {
        return result;
    }
    const bool session = name == "SessionStart" || name == "SessionEnd";
    const bool child = name == "SubagentStart" || name == "SubagentStop";
    const bool tool = name == "PreToolUse" || name == "PostToolUse" || name == "PostToolUseFailure";
    if ((!session && !child && body.find("agent_id")) ||
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
    observation.provider_session_id = metadata(body, "session_id");
    result.classification = addon::Classification::EVENT;
    if (name == "SessionStart") {
        observation.session_source = metadata(body, "source");
        observation.session_transition = addon::Session_transition::STARTED;
        result.classification = addon::Classification::TRANSITION;
        result.transition = observation.session_source == "resume" ? addon::Transition_kind::RESUME : addon::Transition_kind::START;
        if (canonical_session_id(observation.provider_session_id)) {
            auto record = addon::Json_value::object();
            record.set("provider_session_id", addon::Json_value(observation.provider_session_id));
            observation.resume_record = std::move(record);
        }
    }
    else
    if (name == "SessionEnd") {
        observation.session_transition = addon::Session_transition::ENDED;
        observation.session_end_reason = metadata(body, "reason");
    }
    else
    if (child) {
        observation.agent_id = metadata(body, "agent_id");
        observation.agent_label = metadata(body, "agent_type");
        observation.subagent_transition = name == "SubagentStart" ? addon::Subagent_transition::STARTED : addon::Subagent_transition::STOPPED;
    }
    else {
        if (name == "ModTurnComplete") {
            observation.phase_transition = addon::Phase_transition::AWAITING_INPUT;
            observation.phase_evidence = addon::Evidence::CONFIRMED;
            result.observations.push_back(std::move(observation));
            return result;
        }
        if (body.find("prompt_id")) observation.provider_turn_id = metadata(body, "prompt_id");
        if (name == "UserPromptSubmit" || name == "Stop" || name == "StopFailure") {
            observation.phase_transition = name == "UserPromptSubmit" ? addon::Phase_transition::WORKING : addon::Phase_transition::AWAITING_INPUT;
            observation.phase_evidence = addon::Evidence::INFERRED;
            if (name == "StopFailure" && !observation.provider_turn_id.empty()) {
                (void)metadata(body, "error");
                observation.outcome_transition = addon::Outcome_transition::FAILED;
                observation.outcome_evidence = addon::Evidence::CONFIRMED;
            }
        }
        else {
            observation.attention_transition = name == "PostToolUse" || name == "PostToolUseFailure"
                ? addon::Attention_transition::RESOLVED : addon::Attention_transition::REQUESTED;
            observation.attention_evidence = addon::Evidence::INFERRED;
            if (tool) observation.attention_request_id = metadata(body, "tool_use_id");
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
            context.fail(addon::Result_status::FAILED, failure.reason_code);
        }
        else {
            context.succeed(addon::encode_attention_classification_result(classify_attention(*request)));
        }
        return;
    }
    const auto request = addon::decode_classify_event_request(context.request().payload, &failure);
    if (!request) {
        context.fail(addon::Result_status::FAILED, failure.reason_code);
        return;
    }
    context.succeed(addon::encode_classify_event_result(classify_event(*request)));
}

} // namespace claude
