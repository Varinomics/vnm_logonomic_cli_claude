#include "claude_lifecycle.h"
#include "claude_support.h"

#include <logonomic_cli_addon/capability_ids.h>
#include <logonomic_cli_addon/execution_context.h>
#include <logonomic_cli_addon/lifecycle_events.h>

#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
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

void execution_context_contract()
{
    // Oracle: the native SessionStart common cwd field is an exact snapshot,
    // independent of the conversation identity and of its containing Git root.
    // Issue #12 requires unknown cwd to retain the real identity transition.
    addon::Classify_event_request request;
    request.execution_context_version = addon::k_execution_context_version;
    request.transition_nonce = std::string(32, '1');
    request.event.name = "SessionStart";
    request.event.body = claude::object(R"({"hook_event_name":"SessionStart",
        "session_id":"a375d05e-f6cf-4812-80f5-91b33d24c053","source":"resume"})");
#ifdef _WIN32
    const std::vector<std::string> paths{R"(C:\work\repo\nested\directory)", R"(C:\not a repository\notes )"};
#else
    const std::vector<std::string> paths{"/work/repo/nested/directory", "/not a repository/notes "};
#endif
    for (const auto& cwd : paths) {
        request.event.body.set("cwd", Json(cwd));
        const auto result = claude::classify_event(request);
        require(result.classification == addon::Classification::TRANSITION &&
            result.transition == addon::Transition_kind::RESUME && result.observations.size() == 1 &&
            result.observations.front().resume_record && result.execution_updates.size() == 1 &&
            !result.execution_failure, "identity, exact resume record and cwd remain independent facts");
        const auto& update = result.execution_updates.front();
        require(update.provider_session_id == result.observations.front().provider_session_id &&
            update.transition_nonce == request.transition_nonce && update.sequence == 0 &&
            update.status == addon::Execution_context_status::CONFIRMED && update.environment == "local" &&
            update.cwd == cwd && update.source == "claude.SessionStart",
            "native cwd is preserved without trimming, resolving or substituting a Git root");
        addon::Contract_failure failure;
        const auto decoded = addon::decode_classify_event_result(addon::encode_classify_event_result(result), &failure);
        require(decoded && decoded->execution_updates == result.execution_updates && !decoded->execution_failure &&
            decoded->observations.size() == 1, "execution context round-trips beside native lifecycle evidence");
        require(claude::classify_event(request).execution_updates == result.execution_updates,
            "replaying a delivery retains the original native transition attribution");
    }
    const auto require_unknown = [&request]() {
        const auto result = claude::classify_event(request);
        require(result.classification == addon::Classification::TRANSITION &&
            result.observations.size() == 1 && result.observations.front().resume_record &&
            result.execution_updates.size() == 1 && !result.execution_failure,
            "unavailable cwd cannot discard a genuine conversation transition");
        const auto& update = result.execution_updates.front();
        require(update.status == addon::Execution_context_status::UNKNOWN && update.cwd.empty() &&
            update.environment.empty() && update.transition_nonce == request.transition_nonce,
            "unknown cwd is explicit and scoped to the accepted native transition");
    };
    for (const Json& cwd : {Json(), Json(std::int64_t{12}), Json("relative/path"), Json("ssh://remote/work"),
            Json("/work\nother"), Json(std::string("/work\0other", 11)), Json(std::string(4097, '/'))})
    {
        request.event.body.set("cwd", cwd);
        require_unknown();
    }
    request.event.body = claude::object(R"({"hook_event_name":"SessionStart",
        "session_id":"a375d05e-f6cf-4812-80f5-91b33d24c053","source":"clear",
        "transcript_path":"/storage/transcript.jsonl","project_dir":"/launch/project",
        "workspace":{"current_dir":"/unrelated/input"}})");
    require_unknown();

    // A -> B -> A must use the new delivery's nonce, never the current ledger's
    // nonce or the first activation of the same transcript UUID.
    request.ledger.current_epoch = 3;
    request.ledger.current_transition = addon::Ledger_transition{
        addon::Transition_kind::RESUME, "b375d05e-f6cf-4812-80f5-91b33d24c053", std::string(32, '2')};
    request.transition_nonce = std::string(32, '3');
    request.event.body.set("source", Json("resume"));
    require_unknown();
    request.event.body.set("cwd", Json(paths.front()));
    request.execution_context_version = 0;
    auto result = claude::classify_event(request);
    require(result.classification == addon::Classification::TRANSITION && result.execution_updates.empty() &&
        !addon::encode_classify_event_result(result).find("execution_context"),
        "unnegotiated legacy lifecycle responses omit the extension");
    request.execution_context_version = addon::k_execution_context_version;
    auto malformed = addon::encode_classify_event_request(request);
    malformed.set("execution_context", claude::object(R"({"version":"1"})"));
    addon::Contract_failure failure;
    const auto decoded = addon::decode_classify_event_request(malformed, &failure);
    require(decoded && decoded->execution_failure, "invalid optional request remains independently decodable");
    result = claude::classify_event(*decoded);
    require(result.classification == addon::Classification::TRANSITION && result.observations.size() == 1 &&
        result.execution_updates.empty() && result.execution_failure,
        "invalid execution negotiation cannot discard native identity");

    request.event.body.set("source", Json("compact"));
    result = claude::classify_event(request);
    require(result.classification == addon::Classification::IGNORED && result.execution_updates.empty(),
        "compaction cannot create an execution activation or substitute a start snapshot");
    request.event.body.set("source", Json("startup"));
    request.event.body.set("agent_id", Json("child"));
    result = claude::classify_event(request);
    require(result.classification == addon::Classification::IGNORED && result.execution_updates.empty() &&
        result.observations.empty(), "child identity and cwd cannot replace the root session");
    request.event.name = "SessionEnd";
    request.event.body.set("hook_event_name", Json("SessionEnd"));
    result = claude::classify_event(request);
    require(result.classification == addon::Classification::IGNORED && result.observations.empty(),
        "a child SessionEnd cannot close the root session");
    request.event.name = "CwdChanged";
    request.event.body = claude::object(R"({"hook_event_name":"CwdChanged",
        "session_id":"a375d05e-f6cf-4812-80f5-91b33d24c053","new_cwd":"/work/other"})");
    result = claude::classify_event(request);
    require(result.classification == addon::Classification::IGNORED && result.execution_updates.empty(),
        "uncorrelated standalone cwd events cannot borrow an activation nonce at delivery time");
}

} // namespace

int main()
{
    try {
        execution_context_contract();
        std::cout << "Claude execution context contract checks passed.\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
