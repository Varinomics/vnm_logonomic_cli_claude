#include "claude_lifecycle.h"
#include "claude_sessions.h"
#include "claude_support.h"

#include <logonomic_cli_addon/capability_ids.h>
#include <logonomic_cli_addon/model_state.h>

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace addon = logonomic::cli_addon;
using Json = addon::Json_value;

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void lifecycle_model()
{
    // Oracle: the native SessionStart model is a live session fact. The same
    // accepted transition supplies its nonce; no backend follows from its name.
    addon::Classify_event_request request;
    request.transition_nonce = std::string(32, 'a');
    request.event = {"SessionStart", claude::object(R"({"hook_event_name":"SessionStart",
        "session_id":"session","source":"startup","model":"gateway/root-current"})")};
    require(claude::classify_event(request).model_updates.empty(), "Unrequested model remains absent");
    request.model_state_version = addon::k_model_state_version;
    auto result = claude::classify_event(request);
    require(result.classification == addon::Classification::TRANSITION && result.observations.size() == 1 &&
        result.model_updates.size() == 1 && !result.model_failure, "Start carries independent model evidence");
    const auto& update = result.model_updates.front();
    require(update.provider_session_id == "session" && update.model == "gateway/root-current" &&
        update.provider.empty() && update.agent_id.empty() && update.sequence == 0 &&
        update.transition_nonce == request.transition_nonce, "Root uses only its own literal native source");
    for (const auto& model : {Json(""), Json("model\ncontrol"), Json(std::string(513, 'x')), Json(12)}) {
        request.event.body.set("model", model);
        result = claude::classify_event(request);
        require(result.classification == addon::Classification::TRANSITION && result.observations.size() == 1 &&
            result.model_updates.empty() && result.model_failure, "Malformed model preserves lifecycle");
    }
    request.event.body.set("model", Json("child-model"));
    request.event.body.set("source", Json("compact"));
    require(claude::classify_event(request).model_updates.empty(), "Compaction never supplies root model");
    request.event.body.set("source", Json("startup"));
    request.event.body.set("agent_id", Json("child"));
    require(claude::classify_event(request).model_updates.empty(), "Child start cannot supply parent model");

    // The in-process source captures its activation and order before an async
    // native step. A later delivery must not borrow the request/ledger nonce.
    addon::Model_update native;
    native.provider_session_id = "session";
    native.transition_nonce = std::string(32, 'b');
    native.sequence = 7;
    native.status = addon::Model_status::CONFIRMED;
    native.model = "gateway/child-served";
    native.agent_id = "child";
    native.source = "claude.turn.step.usage";
    request.event = {"ModModel", claude::object(R"({"hook_event_name":"ModModel",
        "session_id":"session","agent_id":"child"})")};
    addon::encode_model_updates(request.event.body, {native}, {});
    result = claude::classify_event(request);
    require(result.classification == addon::Classification::IGNORED && result.observations.empty() &&
        result.model_updates == std::vector{native}, "Model telemetry creates no lifecycle or attention row");
    const auto decoded = addon::decode_classify_event_result(addon::encode_classify_event_result(result), nullptr);
    require(decoded && decoded->model_updates == result.model_updates && !decoded->model_failure,
        "Native source activation survives the actual carrier");
    request.event.body.set("agent_id", Json("other-child"));
    result = claude::classify_event(request);
    require(result.model_updates.empty() && result.model_failure && result.observations.empty(),
        "Mismatched actor fails only the optional capability");
    request.event.body.set("agent_id", Json("child"));
    native.source = "claude.PostModelSwitch";
    addon::encode_model_updates(request.event.body, {native}, {});
    require(claude::classify_event(request).model_failure.has_value(), "Session model switch cannot label a child");
    native.source = "claude.turn.step.usage";
    native.sequence = 0;
    addon::encode_model_updates(request.event.body, {native}, {});
    require(claude::classify_event(request).model_failure.has_value(), "Standalone source cannot claim joint start order");
    request.model_state_version = 0;
    result = claude::classify_event(request);
    require(result.model_updates.empty() && !result.model_failure, "Legacy caller receives no model extension");
}

std::string initial_state()
{
    return R"({"initialized":false,"pending_base64":"","result_base64":"",
        "root_session_id":"","working_directory":"/repo","task_tools":[],"expected_mcp_servers":[]})";
}

addon::Structured_inspect_result inspect(std::string state, std::string bytes,
    bool final = false, bool observe = true, int exit_code = 0)
{
    addon::Structured_inspection request;
    request.protocol_state = std::move(state);
    request.stdout_bytes = std::move(bytes);
    request.final = final;
    request.exit_code = exit_code;
    return claude::inspect_session(addon::Structured_purpose::IMPLEMENTATION, request, observe);
}

void structured_model()
{
    // Oracle: Claude stream-json init and root assistant messages are native
    // sources. Result.modelUsage is an aggregate and cannot identify an actor.
    const std::string init = R"({"type":"system","subtype":"init","session_id":"root","model":"native-live"})" "\n";
    const std::string answer = R"({"type":"result","subtype":"success","session_id":"root",
        "is_error":false,"result":"done","modelUsage":{"aggregate-only":{}}})" "\n";
    const std::string assistant = R"({"type":"assistant","session_id":"root","parent_tool_use_id":null,
        "message":{"role":"assistant","model":"native-served"}})" "\n";
    auto result = inspect(initial_state(), init + assistant + answer, true, false);
    require(result.complete && !result.failure_reason && result.result_bytes == "done" && !result.model_observation,
        "Legacy inspection remains independent of model capability");
    result = inspect(initial_state(), init);
    require(result.model_observation && result.model_observation->model == "native-live" &&
        result.model_observation->provider.empty(), "Init supplies its own literal live model without a backend guess");
    result = inspect(result.protocol_state, assistant + answer, true);
    require(result.complete && !result.failure_reason && result.result_bytes == "done" && result.model_observation &&
        result.model_observation->model == "native-served" && result.model_observation->source == "claude.assistant.message",
        "An own served response replaces earlier live evidence and ignores aggregate usage");

    const std::vector<std::string> excluded{
        R"({"type":"assistant","session_id":"child","message":{"role":"assistant","model":"child"}})",
        R"({"type":"assistant","session_id":"root","parent_tool_use_id":"tool","message":{"role":"assistant","model":"child"}})",
        R"({"type":"assistant","session_id":"root","isSynthetic":true,"message":{"role":"assistant","model":"synthetic"}})",
        R"({"type":"assistant","session_id":"root","isCompactSummary":true,"message":{"role":"assistant","model":"summary"}})",
        R"({"type":"assistant","session_id":"root","message":{"role":"assistant","model":"<synthetic>"}})",
        R"({"type":"system","subtype":"compact_boundary","session_id":"root","model":"compaction"})",
    };
    for (const auto& event : excluded) {
        result = inspect(initial_state(), init + event + '\n' + answer, true);
        require(result.complete && !result.failure_reason && result.model_observation &&
            result.model_observation->model == "native-live", "Child, synthetic and compaction records cannot relabel root");
    }
    const auto split = assistant.size() / 2;
    auto first = inspect(initial_state(), init + assistant.substr(0, split));
    auto idle = inspect(first.protocol_state, "");
    result = inspect(idle.protocol_state, assistant.substr(split) + answer, true);
    require(result.model_observation && result.model_observation->model == "native-served" && !result.failure_reason,
        "Chunk boundaries retain native attribution without reinterpreting partial JSON");
    result = inspect(initial_state(), init + assistant + answer, true, true, 7);
    require(result.failure_reason == "process_failed" && result.model_observation &&
        result.model_observation->model == "native-served", "A later process failure does not erase observed native evidence");
    const std::string malformed = R"({"type":"assistant","session_id":"root","message":{"role":"assistant","model":17}})" "\n";
    result = inspect(initial_state(), init + malformed + answer, true);
    require(result.complete && !result.failure_reason && result.result_bytes == "done" && !result.model_observation &&
        result.model_failure, "Malformed optional native model preserves authenticated completion");
    auto envelope = addon::encode_structured_inspect_result(result);
    auto decoded = addon::decode_structured_inspect_result(envelope, nullptr);
    require(decoded && decoded->model_failure && decoded->result_bytes == "done",
        "Optional model failure survives the actual structured carrier");
}

} // namespace

int main()
{
    try {
        lifecycle_model();
        structured_model();
        return 0;
    }
    catch (const std::exception& failure) {
        std::cerr << "Claude model contract: " << failure.what() << '\n';
        return 1;
    }
}
