#include "claude_lifecycle.h"
#include "claude_support.h"

#include <logonomic_cli_addon/limits.h>

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <string_view>

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
    const std::array<std::string_view, 11> supported{
        "SessionStart", "SessionEnd", "SubagentStart", "SubagentStop", "UserPromptSubmit", "Stop",
        "StopFailure", "PermissionRequest", "PreToolUse", "PostToolUse", "PostToolUseFailure",
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

void handle_lifecycle(addon::Invocation_context& context)
{
    if (context.request().operation != addon::k_classify_event_operation) {
        context.reject_unknown_operation();
        return;
    }
    addon::Contract_failure failure;
    const auto request = addon::decode_classify_event_request(context.request().payload, &failure);
    if (!request) {
        context.fail(addon::Result_status::FAILED, failure.reason_code);
        return;
    }
    context.succeed(addon::encode_classify_event_result(classify_event(*request)));
}

} // namespace claude
