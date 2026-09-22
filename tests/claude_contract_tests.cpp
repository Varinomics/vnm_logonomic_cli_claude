#include "claude_accounts.h"
#include "claude_launcher.h"
#include "claude_lifecycle.h"
#include "claude_preparation.h"
#include "claude_probes.h"
#include "claude_sessions.h"
#include "claude_support.h"
#include "claude_usage.h"
#include <logonomic_cli_addon/sha256.h>

#include <logonomic_cli_addon/legacy_state_import.h>
#include <logonomic_cli_addon/launch_preparation.h>
#include <logonomic_cli_addon/accounts.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/probe_contracts.h>
#include <logonomic_cli_addon/session_contracts.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
using Json = addon::Json_value;

class Context : public addon::Invocation_context
{
public:
    const addon::Invoke_request& request() const override { return m_request; }
    bool cancel_requested() const override { return m_cancelled; }
    bool wait_for_cancel(int) override { return m_cancelled; }
    void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override {}
    void succeed(Json payload) override { m_payload = std::move(payload); m_finished = true; }
    void fail(addon::Result_status status, std::string reason, std::string) override
    {
        m_status = status;
        m_reason = std::move(reason);
        m_finished = true;
    }
    bool finished() const override { return m_finished; }
    addon::Invoke_request m_request;
    Json m_payload;
    addon::Result_status m_status = addon::Result_status::SUCCESS;
    std::string m_reason;
    bool m_cancelled = false;
    bool m_finished = false;
};

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

Json protocol(bool review)
{
    auto result = claude::object(R"({"initialized":false,"pending_base64":"","result_base64":"","root_session_id":"","working_directory":"","expected_tools":["Read","Glob","Grep"],"expected_mcp_servers":[]})");
    if (review) result.set("working_directory", Json(claude::path_string(fs::current_path())));
    return result;
}

void launcher_contract()
{
    // Native recognition accepts executable/package identity, never a mention.
    require(claude::recognize_launcher("claude --model sonnet").has_value(), "direct Claude recognition");
    require(claude::recognize_launcher("npx --yes @anthropic-ai/claude-code@2.1.0").has_value(), "npx recognition");
    require(claude::recognize_launcher("bunx @anthropic-ai/claude-code").has_value(), "bunx recognition");
    require(!claude::recognize_launcher("echo claude"), "a mention is not a launcher");
    require(!claude::recognize_launcher("npx --package other @anthropic-ai/claude-code"), "unknown runner option");
    require(claude::interactive_resume_arguments({"claude", "--model", "sonnet"}), "stock resume flags");
    require(!claude::interactive_resume_arguments({"claude", "--resume", "already-selected"}), "resume cannot overwrite existing selector");
    require(!claude::interactive_resume_arguments({"claude", "-p", "existing prompt"}), "resume cannot steal print input");
    const auto prepared = claude::prepared_argv({"claude", "--append-system-prompt", "User instruction"},
        "Logonomic instruction", std::string("C:/plugin root"), {});
    require(prepared && (*prepared)[2] == "User instruction Logonomic instruction", "preserve and append user instruction");
    require(prepared->at(prepared->size() - 2) == "--plugin-dir", "plugin flag appended");
    const auto shell = claude::prepared_argv({"powershell.exe", "-NoProfile", "-Command", "claude --model sonnet"},
        "Owner's instruction", std::string("C:/Owner's plugin"), {});
    require(shell && (*shell)[1] == "-NoProfile" && shell->back().find("Owner''s") != std::string::npos,
        "PowerShell prefix and apostrophes preserved");
    require(!claude::prepared_argv({"bash", "-c", "claude; echo done"}, "Instruction", std::nullopt, {}),
        "control syntax is not rewritten");
}

void launcher_directory_contract()
{
    const auto root = fs::temp_directory_path() / ("claude-path-" + addon::random_nonce_hex());
    fs::create_directories(root / "tools");
    try {
        addon::Launch_environment environment;
#ifdef _WIN32
        environment.platform_id = "windows-x86_64";
        const auto node = root / "tools/node.exe";
        const auto program = root / "tools/claude.exe";
#else
        environment.platform_id = "linux-x86_64";
        const auto node = root / "tools/node";
        const auto program = root / "tools/claude";
#endif
        std::ofstream(node).close();
        std::ofstream(program).close();
        environment.effective_environment = Json::object();
        environment.effective_environment.set("PATH", Json("tools"));
        const auto directory_bytes = root.generic_u8string();
        const std::string directory(reinterpret_cast<const char*>(directory_bytes.data()), directory_bytes.size());
        require(claude::node_available(environment, directory), "relative Node PATH resolves against requested directory");
        require(!claude::node_available(environment, directory + "/missing"), "Node lookup never borrows adapter cwd");
        addon::Session_launch_context launch;
        launch.working_directory = directory;
        launch.launch_environment = environment;
        launch.launcher.script_body = "claude";
        launch.launcher.runner_key = "bash";
        launch.launcher_argv = {"bash", "-c", "claude"};
        const auto process = claude::cli_process(launch, false);
        require(!process.argv.empty() && addon::path_from_utf8(process.argv.front()) == program,
            "wrapped Claude relative PATH resolves against requested directory");
#ifdef _WIN32
        // Windows environment names are case-insensitive, and the host passes
        // the launcher environment in its own spelling: `Path` for the system
        // search path.
        auto windows_spelling = launch;
        auto& effective = windows_spelling.launch_environment->effective_environment;
        effective.erase("PATH");
        effective.set("Path", Json("tools"));
        const auto resolved = claude::cli_process(windows_spelling, false);
        require(!resolved.argv.empty() && addon::path_from_utf8(resolved.argv.front()) == program,
            "the Windows Path spelling resolves the wrapped Claude program");
        require(claude::node_available(*windows_spelling.launch_environment, directory),
            "the Windows Path spelling finds Node for hooks and resume");
#endif
        require(claude::canonical_resume_directory(directory), "clean absolute resume directory admitted");
        addon::Resume_prepare_request resume;
        resume.context = launch;
        resume.context.launch_environment->effective_environment.set(
            "CLAUDE_CONFIG_DIR", Json(claude::path_string(root / "selected-profile")));
        resume.title_instruction = "Set the session title.";
        resume.resume_record = claude::object(
            R"({"provider_session_id":"a375d05e-f6cf-4812-80f5-91b33d24c053"})");
        Context invocation;
        invocation.m_request.capability.capability_id = "interactive-resume";
        invocation.m_request.operation = "prepare_resume";
        invocation.m_request.payload = addon::encode_resume_prepare_request(resume, 2);
        claude::handle_preparation(invocation, root);
        require(invocation.m_finished && invocation.m_status == addon::Result_status::SUCCESS,
            "valid interactive resume reaches the actual producer");
        addon::Contract_failure failure;
        const auto resumed = addon::decode_resume_prepare_result(invocation.m_payload, &failure, 2);
        require(resumed && addon::validate_resume_process_proposal(resumed->process, &failure) &&
            addon::validate_launch_patch(resumed->patch, {}, {}, &failure),
            "produced resume satisfies host process and environment patch policy");
        require(resumed->process.environment.empty() && resumed->process.environment_names.empty() &&
            resumed->process.assets.empty(), "resume environment belongs exclusively to the launch patch");
        const std::vector<std::pair<std::string, std::string>> expected_environment{
            {"CLAUDE_CONFIG_DIR", claude::path_string(root / "selected-profile")},
            {"CLAUDE_CODE_SESSIONEND_HOOKS_TIMEOUT_MS", "5000"},
        };
        require(resumed->patch.set == expected_environment,
            "resume patch retains the frozen profile and hook timeout");
        require(!claude::canonical_resume_directory(directory + "/."), "dot resume directory refused");
        require(!claude::canonical_resume_directory(directory + "/tools/.."), "parent traversal resume directory refused");
        require(!claude::canonical_resume_directory(directory + "/"), "non-root trailing separator refused");
#ifdef _WIN32
        auto native = directory;
        for (char& ch : native) if (ch == '/') ch = '\\';
        require(!claude::canonical_resume_directory(native), "resume requires native oracle forward-separator spelling");
#endif
    }
    catch (...) {
        fs::remove_all(root);
        throw;
    }
    fs::remove_all(root);
}

void lifecycle_contract()
{
    for (const auto& [tool_name, kind] : std::vector<std::pair<std::string, std::string>>{
            {"ExitPlanMode", "permission"}, {"AskUserQuestion", "interactive_question"}})
    {
        for (const std::string event_name : {"PreToolUse", "PostToolUse", "PostToolUseFailure"}) {
            addon::Attention_classification_request request;
            request.base.event.name = event_name;
            request.base.event.body = claude::object(R"({"session_id":"session","tool_use_id":"native-tool-request"})");
            request.base.event.body.set("hook_event_name", Json(event_name));
            request.base.event.body.set("tool_name", Json(tool_name));
            const auto result = claude::classify_attention(request);
            require(result.attention_kind && result.attention_kind->kind == kind &&
                result.attention_kind->observation_index == 0,
                "plan approval and interactive question retain distinct kinds on request and resolution");
            require(result.base.observations.size() == 1, "native tool event has one observation");
            const auto& observation = result.base.observations.front();
            require(observation.provider_event_name == event_name &&
                observation.attention_request_id == "native-tool-request",
                "attention classification preserves native event and paired tool identity");
            require(observation.attention_transition == (event_name == "PreToolUse"
                    ? addon::Attention_transition::REQUESTED : addon::Attention_transition::RESOLVED),
                "native pre and post events request and resolve the same attention");
        }
    }
    {
        addon::Attention_classification_request request;
        request.base.event.name = "Stop";
        request.base.event.body = claude::object(R"({"hook_event_name":"Stop","session_id":"session","last_assistant_message":"Which directory?"})");
        auto result = claude::classify_attention(request);
        require(result.attention_kind && result.attention_kind->kind == "conversational_hint",
            "ordinary complete assistant question gives inferred hint");
        const auto id = result.base.observations.front().attention_request_id;
        require(!id.empty() && result.base.observations.front().provider_event_name == "Stop",
            "hint has its own identity without rewriting native Stop");
        require(claude::classify_attention(request).base.observations.front().attention_request_id == id,
            "replayed event has stable hint identity");
        require(claude::classify_event(request.base).observations.front().attention_transition == addon::Attention_transition::NONE,
            "ordinary helper ingress does not acquire companion behavior");
        request.base.event.body.set("agent_id", Json("child"));
        require(!claude::classify_attention(request).attention_kind, "child message cannot raise root hint");
        request.base.event.name = "UserPromptSubmit";
        request.base.event.body = claude::object(R"({"hook_event_name":"UserPromptSubmit","session_id":"session"})");
        request.pending_conversational_request_id = id;
        result = claude::classify_attention(request);
        require(result.base.observations.front().attention_transition == addon::Attention_transition::RESOLVED &&
            result.base.observations.front().attention_request_id == id,
            "actual submission resolves only the host supplied conversational identity");
        request.pending_conversational_request_id.reset();
        require(!claude::classify_attention(request).attention_kind, "submission without pending hint invents no resolution");
    }
    addon::Classify_event_request request;
    request.event.name = "SessionStart";
    request.event.body = claude::object(R"({"hook_event_name":"SessionStart","session_id":"a375d05e-f6cf-4812-80f5-91b33d24c053","source":"resume"})");
    const auto start = claude::classify_event(request);
    require(start.classification == addon::Classification::TRANSITION && start.transition == addon::Transition_kind::RESUME,
        "native resume source selects resume transition");
    require(start.observations[0].resume_record.has_value(), "canonical Session UUID supplies exact resume record");
    request.event.name = "UserPromptSubmit";
    request.event.body = claude::object(R"({"hook_event_name":"UserPromptSubmit","session_id":"session","prompt_id":"turn"})");
    auto result = claude::classify_event(request);
    require(result.observations[0].phase_evidence == addon::Evidence::INFERRED, "Claude root phase evidence remains inferred");
    request.event.body.set("agent_id", Json("child"));
    require(claude::classify_event(request).classification == addon::Classification::IGNORED, "child root-looking event is ignored");
    request.event.name = "StopFailure";
    request.event.body = claude::object(R"({"hook_event_name":"StopFailure","session_id":"session","error":"provider failure"})");
    result = claude::classify_event(request);
    require(result.observations[0].outcome_transition == addon::Outcome_transition::NONE, "no invented turn correlation");
    request.event.body.set("prompt_id", Json("turn"));
    require(claude::classify_event(request).observations[0].outcome_transition == addon::Outcome_transition::FAILED,
        "native turn correlation authorizes failure");
}

void structured_contract()
{
    addon::Structured_inspection input;
    input.protocol_state = addon::serialize_json(protocol(false));
    input.stdout_bytes = R"({"type":"system","subtype":"init","session_id":"root"})" "\n"
        R"({"type":"result","session_id":"child","subtype":"success","is_error":false,"result":"Child output"})" "\n"
        R"({"type":"result","session_id":"root","subtype":"success","is_error":false,"result":"Root output"})" "\n";
    auto result = claude::inspect_session(addon::Structured_purpose::IMPLEMENTATION, input);
    require(!result.complete && !result.result_bytes, "implementation waits for exit after root result");
    input.protocol_state = result.protocol_state;
    input.stdout_bytes.clear();
    input.final = true;
    result = claude::inspect_session(addon::Structured_purpose::IMPLEMENTATION, input);
    require(result.complete && result.result_bytes == "Root output", "child result cannot replace root result");
    input.exit_code = 1;
    require(claude::inspect_session(addon::Structured_purpose::IMPLEMENTATION, input).failure_reason.has_value(),
        "nonzero exit cannot authenticate result");
    input = {};
    input.protocol_state = addon::serialize_json(protocol(false));
    input.stdout_bytes = R"({"type":"system","subtype":"hook_started","session_id":"one"})" "\n"
        R"({"type":"system","subtype":"init","session_id":"two"})" "\n";
    require(claude::inspect_session(addon::Structured_purpose::IMPLEMENTATION, input).failure_reason.has_value(),
        "pre-init hook provenance must match root");
    auto init = claude::object(R"({"type":"system","subtype":"init","permissionMode":"plan","tools":["Grep","Read","Glob"],"mcp_servers":[],"plugins":[],"cwd":""})");
    init.set("cwd", Json(claude::path_string(fs::current_path())));
    init.set("session_id", Json("root"));
    input = {};
    input.protocol_state = addon::serialize_json(protocol(true));
    input.stdout_bytes = addon::serialize_json(init) + '\n';
    result = claude::inspect_session(addon::Structured_purpose::READ_ONLY_REVIEW, input);
    require(!result.complete && !result.failure_reason, "exact read-only surface admitted");
    const auto review_state = result.protocol_state;
    input.protocol_state = review_state;
    input.stdout_bytes = R"({"type":"result","session_id":"child","subtype":"success","is_error":false,"result":"Child output"})" "\n"
        R"({"type":"result","session_id":"root","parent_tool_use_id":"child-tool","subtype":"success","is_error":false,"result":"Nested output"})" "\n";
    result = claude::inspect_session(addon::Structured_purpose::READ_ONLY_REVIEW, input);
    require(!result.complete && !result.failure_reason && !result.result_bytes,
        "review child results cannot authenticate the root result");
    input.protocol_state = result.protocol_state;
    input.stdout_bytes = R"({"type":"result","session_id":"root","subtype":"success","is_error":false,"result":"Root review"})" "\n";
    result = claude::inspect_session(addon::Structured_purpose::READ_ONLY_REVIEW, input);
    require(result.complete && result.result_bytes == "Root review",
        "review root identity survives incremental inspection");
    input.protocol_state = review_state;
    input.stdout_bytes = R"({"type":"result","subtype":"success","is_error":false,"result":"Unbound review"})" "\n";
    require(claude::inspect_session(addon::Structured_purpose::READ_ONLY_REVIEW, input).failure_reason ==
        "invalid_result_provenance", "review result requires native session identity");
    input.protocol_state = addon::serialize_json(protocol(true));
    init.set("session_id", Json(""));
    input.stdout_bytes = addon::serialize_json(init) + '\n';
    require(claude::inspect_session(addon::Structured_purpose::READ_ONLY_REVIEW, input).failure_reason ==
        "session_provenance_mismatch", "review initialization requires native session identity");
    init.set("session_id", Json("root"));
    init.find("tools")->push_back(Json("Write"));
    input.stdout_bytes = addon::serialize_json(init) + '\n';
    require(claude::inspect_session(addon::Structured_purpose::READ_ONLY_REVIEW, input).failure_reason.has_value(),
        "extra mutating tool invalidates read-only attestation");
}

addon::Delegated_prepare_result delegated_fixture(addon::Structured_purpose purpose)
{
    addon::Delegated_prepare_request request;
    request.purpose = purpose;
    request.prompt = "Inspect the selected task.\nReturn the findings.";
    request.context.launcher.script_body = "claude";
    request.context.launcher.runner_key = "direct";
    request.context.launcher_argv = {"claude"};
    request.context.working_directory = claude::path_string(fs::current_path());
    addon::Launch_environment environment;
#ifdef _WIN32
    environment.platform_id = "windows-x86_64";
#else
    environment.platform_id = "linux-x86_64";
#endif
    environment.effective_environment = Json::object();
    environment.effective_environment.set("CLAUDE_CONFIG_DIR", Json(request.context.working_directory));
    request.context.launch_environment = environment;
    request.control.task_id = "readiness-task";
    request.control.private_artifact_root = claude::path_string(fs::current_path() / "private-readiness");
    for (const auto* id : {"coordination", "delegation"}) {
        addon::Delegated_task_server server;
        server.server_id = id;
        server.program = claude::path_string(fs::current_path() / "task-control");
        server.actions = std::string_view(id) == "coordination" ?
            std::vector<std::string>{"status", "notice"} : std::vector<std::string>{"delegate", "inspect_task"};
        request.control.servers.push_back(std::move(server));
    }
    Context invocation;
    invocation.m_request.capability.capability_id = "delegated-session";
    invocation.m_request.operation = "prepare";
    invocation.m_request.payload = addon::encode_delegated_prepare_request(request, 3);
    claude::handle_sessions(invocation, fs::current_path());
    addon::Contract_failure failure;
    const auto prepared = addon::decode_delegated_prepare_result(invocation.m_payload, &failure, 3);
    require(prepared.has_value(), "delegated native preparation satisfies session@3 codec");
    require(prepared->inspection.keep_stdin_open && prepared->inspection.incremental,
        "readiness retains native input across incremental inspection");
    require(prepared->acknowledgment.server_ids == std::vector<std::string>{"coordination", "delegation"},
        "readiness preserves host control acknowledgment");
    addon::Structured_prepare_request ordinary;
    ordinary.purpose = purpose;
    ordinary.context = request.context;
    ordinary.prompt = request.prompt;
    invocation.m_request.capability.capability_id = "structured-session";
    invocation.m_request.payload = addon::encode_structured_prepare_request(ordinary, 3);
    claude::handle_sessions(invocation, fs::current_path());
    const auto one_shot = addon::decode_structured_prepare_result(invocation.m_payload, &failure, 3);
    require(one_shot && !one_shot->inspection.keep_stdin_open &&
        one_shot->process.stdin_payload == request.prompt,
        "ordinary sessions retain raw one-shot input under session@3");
    return *prepared;
}

std::string control_reply(const std::string& state, Json payload = Json::object())
{
    Json response = Json::object();
    response.set("subtype", Json("success"));
    response.set("request_id", Json(claude::text(claude::object(state), "mcp_request_id")));
    if (claude::text(claude::object(state), "mcp_phase") != "reconnect") {
        response.set("response", std::move(payload));
    }
    Json frame = Json::object();
    frame.set("type", Json("control_response"));
    frame.set("response", std::move(response));
    return addon::serialize_json(frame) + '\n';
}

addon::Structured_inspect_result inspect_readiness(
    addon::Structured_purpose purpose, const std::string& state, const std::string& output)
{
    addon::Structured_inspection inspection;
    inspection.protocol_state = state;
    inspection.stdout_bytes = output;
    auto result = claude::inspect_session(purpose, inspection);
    addon::Contract_failure failure;
    require(addon::decode_structured_inspect_result(
        addon::encode_structured_inspect_result(result, 3), false, &failure, 3).has_value(),
        "readiness output satisfies session@3 codec");
    return result;
}

void delegated_readiness_contract()
{
    for (const auto purpose : {addon::Structured_purpose::READ_ONLY_REVIEW, addon::Structured_purpose::IMPLEMENTATION}) {
        const auto prepared = delegated_fixture(purpose);
        const auto initial = prepared.inspection.protocol_state;
        const auto initialize = claude::object(prepared.process.stdin_payload);
        require(claude::text(initialize, "type") == "control_request" &&
            claude::text(*initialize.find("request"), "subtype") == "initialize" &&
            prepared.process.stdin_payload.find("Inspect the selected task") == std::string::npos,
            "native startup contains only initialize, never task bytes");
        const auto ack = control_reply(initial);
        auto result = inspect_readiness(purpose, initial, ack.substr(0, ack.size() / 2));
        require(!result.complete && !result.stdin_bytes && !result.close_stdin,
            "partial acknowledgment cannot advance readiness");
        result = inspect_readiness(purpose, result.protocol_state, ack.substr(ack.size() / 2));
        require(!result.complete && result.stdin_bytes && !result.close_stdin,
            "initialize acknowledgment starts awaited discovery without task release");
        const auto reconnect_state = result.protocol_state;
        for (const auto* id : {"coordination", "delegation"}) {
            const auto reconnect = claude::object(*result.stdin_bytes);
            require(claude::text(*reconnect.find("request"), "subtype") == "mcp_reconnect" &&
                claude::text(*reconnect.find("request"), "serverName") ==
                    std::string("logonomic-task-readiness-task-") + id,
                "only the next required host server is reconnected");
            const auto waiting = inspect_readiness(purpose, result.protocol_state, "");
            require(!waiting.complete && !waiting.stdin_bytes && !waiting.close_stdin,
                "delayed discovery does not poll or release the prompt");
            result = inspect_readiness(purpose, waiting.protocol_state, control_reply(waiting.protocol_state));
        }
        require(claude::text(*claude::object(*result.stdin_bytes).find("request"), "subtype") == "mcp_status",
            "awaited reconnects precede the live catalog snapshot");
        const auto status_state = result.protocol_state;
        auto catalog = claude::object(R"({"mcpServers":[{"name":"logonomic-task-readiness-task-coordination","status":"connected","tools":[{"name":"status"},{"name":"notice"},{"name":"extra"}]},{"name":"logonomic-task-readiness-task-delegation","status":"connected","tools":[{"name":"delegate"},{"name":"inspect_task"}]},{"name":"unrelated-native-server","status":"disabled"}]})");
        result = inspect_readiness(purpose, status_state, control_reply(status_state, catalog));
        require(!result.complete && !result.failure_reason && result.stdin_bytes && result.close_stdin,
            "positive catalog returns task input and ordered EOF in the same incomplete reply");
        const auto user = claude::object(*result.stdin_bytes);
        require(claude::text(user, "type") == "user" &&
            claude::text(*user.find("message"), "content") == "Inspect the selected task.\nReturn the findings.",
            "task is encoded as the exact native JSON user frame");
        require(!claude::flag(claude::object(result.protocol_state), "initialized"),
            "control acknowledgments do not fabricate native initialization");
        const auto released = result.protocol_state;
        auto early_tail = inspect_readiness(purpose, status_state,
            control_reply(status_state, catalog) + "{\"type\":");
        require(!early_tail.stdin_bytes && !early_tail.close_stdin,
            "an unclassified preprompt tail withholds task release");
        if (purpose == addon::Structured_purpose::IMPLEMENTATION) {
            require(!early_tail.complete && !early_tail.failure_reason &&
                claude::text(claude::object(early_tail.protocol_state), "mcp_phase") == "verified",
                "verified catalog retains the prompt until administrative tail classification");
            const auto held_state = early_tail.protocol_state;
            early_tail = inspect_readiness(purpose, held_state, "\"assistant\"}\n");
            require(early_tail.failure_reason == "unexpected_preprompt_output" && !early_tail.stdin_bytes,
                "a completed model tail still refuses input after a verified catalog");
            addon::Structured_inspection ended;
            ended.protocol_state = held_state;
            ended.final = true;
            ended.timed_out = true;
            require(claude::inspect_session(purpose, ended).failure_reason == "process_timed_out",
                "verified-but-unreleased timeout cannot release the prompt");
            ended.timed_out = false;
            const auto exited = claude::inspect_session(purpose, ended);
            require(exited.failure_reason == "mcp_readiness_incomplete" && !exited.stdin_bytes,
                "verified-but-unreleased process exit cannot release the prompt");
        }
        else {
            require(early_tail.failure_reason == "unexpected_preprompt_output",
                "review has no accepted administrative preprompt tail");
        }
        auto init = claude::object(R"({"type":"system","subtype":"init","session_id":"root","permissionMode":"plan","tools":[],"mcp_servers":[{"name":"logonomic-task-readiness-task-coordination","status":"connected"},{"name":"logonomic-task-readiness-task-delegation","status":"connected"}],"plugins":[],"cwd":""})");
        const auto state = claude::object(released);
        init.set("tools", *state.find("expected_tools"));
        init.set("cwd", *state.find("working_directory"));
        result = inspect_readiness(purpose, released, addon::serialize_json(init) + '\n');
        require(!result.complete && !result.failure_reason && !result.stdin_bytes && !result.close_stdin,
            "actual native initialization follows readiness without a second input release");
        result = inspect_readiness(purpose, result.protocol_state,
            R"({"type":"result","session_id":"child","subtype":"success","is_error":false,"result":"Child output"})" "\n"
            R"({"type":"result","session_id":"root","subtype":"success","is_error":false,"result":"Delegated root output"})" "\n");
        require(!result.complete && !result.failure_reason && !result.result_bytes && !result.stdin_bytes,
            "both delegated purposes retain the authenticated root result until EOF settles");
        addon::Structured_inspection final;
        final.protocol_state = result.protocol_state;
        final.final = true;
        const auto completed = claude::inspect_session(purpose, final);
        require(completed.complete && completed.result_bytes == "Delegated root output" && !completed.failure_reason,
            "final successful exit authenticates the retained delegated root result");
        final.exit_code = 1;
        require(claude::inspect_session(purpose, final).failure_reason == "process_failed",
            "delegated root result cannot override failed process exit after EOF");
        if (purpose == addon::Structured_purpose::READ_ONLY_REVIEW) {
            init.set("permissionMode", Json("dontAsk"));
            require(inspect_readiness(purpose, released, addon::serialize_json(init) + '\n').failure_reason ==
                "review_surface_mismatch", "readiness does not relax review's native surface attestation");
        }

        for (const auto* unexpected : {
                "{\"type\":\"assistant\"}\n", "{\"type\":\"result\"}\n",
                "{\"type\":\"system\",\"subtype\":\"init\",\"session_id\":\"root\"}\n"})
        {
            const auto refused = inspect_readiness(purpose, initial, unexpected);
            require(refused.failure_reason == "unexpected_preprompt_output" && !refused.stdin_bytes,
                "both purposes reject native model/init output before readiness");
        }
        auto wrong = claude::object(ack);
        wrong.find("response")->set("request_id", Json("unrelated"));
        require(inspect_readiness(purpose, initial, addon::serialize_json(wrong) + '\n').failure_reason ==
            "mcp_control_response_mismatch", "uncorrelated native acknowledgment is refused");
        wrong = claude::object(ack);
        wrong.find("response")->set("subtype", Json("error"));
        require(inspect_readiness(purpose, initial, addon::serialize_json(wrong) + '\n').failure_reason ==
            "mcp_control_unavailable", "unsupported native control operation refuses task release");
        require(inspect_readiness(purpose, initial, ack + ack).failure_reason == "unexpected_preprompt_output",
            "duplicate buffered acknowledgments cannot advance unsent requests");
        require(inspect_readiness(purpose, released, control_reply(status_state, catalog)).failure_reason ==
            "mcp_control_response_mismatch", "late status cannot release the task twice");
        for (const auto& phase_state : {initial, reconnect_state, status_state}) {
            for (const int defect : {0, 1, 2, 3}) {
                auto reply = claude::object(control_reply(phase_state));
                auto* response = reply.find("response");
                if (defect == 0) response->erase("response");
                if (defect == 1) response->set("response", Json("not-an-object"));
                if (defect == 2) response->set("subtype", Json("error"));
                if (defect == 3) reply.set("type", Json("assistant"));
                if (defect == 0 && phase_state == reconnect_state) {
                    continue; // The positive reconnect fixture already covers its omitted payload.
                }
                const auto refused = inspect_readiness(purpose, phase_state, addon::serialize_json(reply) + '\n');
                require(refused.failure_reason ==
                    (defect == 3 ? "unexpected_preprompt_output" : "mcp_control_unavailable") && !refused.stdin_bytes,
                    "phase-specific native errors and malformed payloads refuse task release");
            }
        }
        const std::string hook = R"({"type":"system","subtype":"hook_started","session_id":"hook-root"})" "\n";
        const auto administrative = inspect_readiness(purpose, initial, hook);
        if (purpose == addon::Structured_purpose::IMPLEMENTATION) {
            require(!administrative.failure_reason && !administrative.stdin_bytes &&
                claude::text(claude::object(administrative.protocol_state), "root_session_id") == "hook-root",
                "implementation retains its exact startup hook provenance before readiness");
            auto partial_hook = inspect_readiness(purpose, initial, ack + hook.substr(0, 15));
            require(partial_hook.stdin_bytes && !partial_hook.failure_reason,
                "fragmented administrative output does not block an accepted control reply");
            partial_hook = inspect_readiness(purpose, partial_hook.protocol_state, hook.substr(15));
            require(!partial_hook.failure_reason && !partial_hook.stdin_bytes,
                "only the established startup hook can complete across the input boundary");
            const auto complete_hook = inspect_readiness(purpose, status_state,
                control_reply(status_state, catalog) + hook);
            for (const auto split : {std::size_t{1}, std::size_t{15}, hook.size() - 1}) {
                auto deferred = inspect_readiness(purpose, status_state,
                    control_reply(status_state, catalog) + hook.substr(0, split));
                require(!deferred.complete && !deferred.failure_reason && !deferred.stdin_bytes && !deferred.close_stdin,
                    "verified catalog defers task release for every split administrative tail");
                deferred = inspect_readiness(purpose, deferred.protocol_state, hook.substr(split));
                require(!deferred.failure_reason && deferred.stdin_bytes == complete_hook.stdin_bytes &&
                    deferred.close_stdin && deferred.protocol_state == complete_hook.protocol_state,
                    "administrative stream chunk boundaries do not change readiness disposition");
            }
        }
        else {
            require(administrative.failure_reason == "unexpected_preprompt_output",
                "review does not acquire implementation startup-hook tolerance");
        }
        auto partial_model = inspect_readiness(purpose, initial, ack + "{\"type\":");
        require(partial_model.stdin_bytes && !partial_model.close_stdin,
            "partial preprompt output cannot release task input");
        partial_model = inspect_readiness(purpose, partial_model.protocol_state, "\"assistant\"}\n");
        require(partial_model.failure_reason == "unexpected_preprompt_output" && !partial_model.stdin_bytes,
            "fragmentation cannot turn early model output into post-input output");
        for (const int defect : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13}) {
            auto invalid = catalog;
            auto& servers = invalid.find("mcpServers")->as_array();
            if (defect == 0) servers.erase(servers.begin() + 1);
            if (defect == 1) servers[0].set("status", Json("failed"));
            if (defect == 2) servers[0].set("tools", Json());
            if (defect == 3) servers[0].set("tools", claude::object(R"({"value":[]})"));
            if (defect == 4) servers[0].find("tools")->as_array().erase(servers[0].find("tools")->as_array().begin());
            if (defect == 5) servers.push_back(servers[0]);
            if (defect == 6) servers[0].find("tools")->push_back(servers[0].find("tools")->as_array()[0]);
            if (defect == 7) servers[0].erase("tools");
            if (defect == 8) servers[0].find("tools")->push_back(Json("not-a-tool"));
            if (defect == 9) servers[0].find("tools")->push_back(Json::object());
            if (defect == 10) servers.push_back(Json("not-a-server"));
            if (defect == 11) invalid.erase("mcpServers");
            if (defect == 12) servers[1].set("tools", *servers[0].find("tools"));
            if (defect == 13) invalid.set("mcpServers", Json::object());
            const auto refused = inspect_readiness(purpose, status_state, control_reply(status_state, invalid));
            require(refused.failure_reason == "required_mcp_unavailable" && !refused.stdin_bytes && !refused.close_stdin,
                "incomplete or ambiguous live catalog never releases task bytes");
        }
        addon::Structured_inspection ended;
        ended.protocol_state = initial;
        ended.final = true;
        ended.timed_out = true;
        const auto timeout = claude::inspect_session(purpose, ended);
        require(timeout.failure_reason == "process_timed_out" && !timeout.stdin_bytes,
            "readiness deadline closes without task release");
        ended.timed_out = false;
        require(claude::inspect_session(purpose, ended).failure_reason == "mcp_readiness_incomplete",
            "clean native exit without readiness cannot authenticate the task");
    }
}

void probe_contract()
{
    addon::Structured_inspection input;
    input.final = true;
    input.stdout_bytes = R"({"loggedIn":true})";
    require(claude::inspect_auth(input) == addon::Auth_status::AUTHENTICATED, "auth requires matching successful exit");
    input.exit_code = 1;
    require(claude::inspect_auth(input) == addon::Auth_status::UNKNOWN, "contradictory auth evidence stays unknown");
    input.stdout_bytes = R"({"loggedIn":false})";
    require(claude::inspect_auth(input) == addon::Auth_status::NOT_LOGGED_IN, "native logged-out exit semantics");
    input.exit_code = 0;
    input.stdout_bytes = R"({"type":"result","subtype":"success","is_error":false,"result":"Current session: 35% used, resets 3pm (Europe/Berlin)\nCurrent week: 21.5% used, resets September 17."})";
    const auto quota = claude::inspect_quota(input);
    require(quota.state == addon::Quota_status::READY && quota.primary->used_percent == 35 &&
        quota.secondary->used_percent == 21.5, "both native quota windows parsed");
    require(quota.secondary->reset_text == "September 17", "text reset retained without guessing epoch");
    input.stdout_bytes = R"({"type":"result","subtype":"success","is_error":false,"result":"Current session: 135.5% used, resets 3pm\nCurrent week: 121% used, resets September 17."})";
    const auto overage = claude::inspect_quota(input);
    require(overage.state == addon::Quota_status::READY && overage.primary->used_percent == 135.5 &&
        overage.secondary->used_percent == 121, "native quota overage remains observable without clamping");
}

#ifdef _WIN32
void probe_profile_contract()
{
    // Windows environment names are case-insensitive, and the host passes the
    // launcher environment in its own spelling. Claude Code honours
    // Claude_Config_Dir, so the quota probe and the account must select it too.
    const auto root = fs::temp_directory_path() / ("claude-profile-" + addon::random_nonce_hex());
    const auto configured = root / "work-profile";
    const auto home = root / "home";
    fs::create_directories(root / "tools");
    fs::create_directories(configured);
    fs::create_directories(home / ".claude");
    std::ofstream(root / "tools/claude.exe").close();
    try {
        addon::Probe_prepare_request request;
        request.context.working_directory = claude::path_string(root);
        request.context.launcher.script_body = "claude";
        request.context.launcher.runner_key = "bash";
        request.context.launcher_argv = {"bash", "-c", "claude"};
        request.context.launch_environment = addon::Launch_environment{};
        request.context.launch_environment->platform_id = "windows-x86_64";
        auto& environment = request.context.launch_environment->effective_environment;
        environment.set("Path", Json(claude::path_string(root / "tools")));
        environment.set("UserProfile", Json(claude::path_string(home)));
        environment.set("Claude_Config_Dir", Json(claude::path_string(configured)));
        addon::Contract_failure failure;
        const auto probe_profile = [&]() {
            Context invocation;
            invocation.m_request.capability.capability_id = "quota";
            invocation.m_request.operation = "prepare";
            invocation.m_request.payload = addon::encode_probe_prepare_request(request, 2);
            claude::handle_probes(invocation, root);
            const auto prepared = addon::decode_structured_prepare_result(invocation.m_payload, &failure, 2);
            require(prepared.has_value(), "quota preparation yields a process proposal");
            for (const auto& [name, value] : prepared->process.environment) {
                if (name == "CLAUDE_CONFIG_DIR") return claude::account_key(addon::path_from_utf8(value));
            }
            throw std::runtime_error("quota probe names no Claude profile");
        };
        const auto resolved_account = [&]() {
            addon::Accounts_launcher_request accounts;
            accounts.launcher = request.context.launcher;
            accounts.launch_environment = *request.context.launch_environment;
            accounts.normalization_directory = claude::path_string(root);
            Context invocation;
            invocation.m_request.capability.capability_id = "accounts";
            invocation.m_request.operation = "resolve";
            invocation.m_request.payload = addon::encode_accounts_launcher_request(accounts);
            claude::handle_accounts(invocation);
            const auto resolution = addon::decode_account_resolution_result(invocation.m_payload, &failure);
            require(resolution && resolution->adapter_account_key, "account resolution names an account");
            return *resolution->adapter_account_key;
        };
        require(probe_profile() == claude::account_key(configured),
            "the quota probe runs against the profile named by Claude_Config_Dir");
        require(resolved_account() == claude::account_key(configured),
            "the account is the profile named by Claude_Config_Dir");
        environment.erase("Claude_Config_Dir");
        require(probe_profile() == claude::account_key(home / ".claude"),
            "without a profile override the quota probe uses UserProfile's .claude");
        require(resolved_account() == claude::account_key(home / ".claude"),
            "without a profile override the account is UserProfile's .claude");
    }
    catch (...) {
        fs::remove_all(root);
        throw;
    }
    fs::remove_all(root);
}
#endif

void usage_contract()
{
    const auto root = fs::temp_directory_path() / ("claude-usage-" + addon::random_nonce_hex());
    fs::create_directories(root / "projects/project");
    try {
        std::ofstream file(root / "projects/project/session.jsonl", std::ios::binary);
        file << R"({"type":"assistant","sessionId":"SessionCase","timestamp":"2026-09-12T12:00:00","message":{"id":"message-A","stop_reason":"end_turn","usage":{"input_tokens":12,"output_tokens":8,"cache_read_input_tokens":3,"cache_creation_input_tokens":2}}})" << '\n';
        file << R"({"type":"assistant","sessionId":"SessionCase","timestamp":"2026-09-12T12:00:00","message":{"id":"message-A","stop_reason":"end_turn","usage":{"input_tokens":12,"output_tokens":8,"cache_read_input_tokens":3,"cache_creation_input_tokens":2}}})" << '\n';
        file << R"({"type":"assistant","sessionId":"SessionCase","timestamp":"2026-09-12T12:00:00","message":{"id":"message-B","stop_reason":"end_turn","usage":{"input_tokens":7,"output_tokens":5}}})" << '\n';
        file << "{unfinished";
        file.close();
        Context context;
        const auto sessions = claude::scan_usage(root, context);
        require(sessions.size() == 1 && sessions[0].record.record_id == "SessionCase" && sessions[0].record.total_tokens == 37,
            "session aggregation deduplicates message id and excludes partial tail");
        require(sessions[0].message_count == 2 && sessions[0].category_coverage[2] == 1 && sessions[0].category_values[2] == 3,
            "partial cache coverage retained separately from observed zero");
        require(sessions[0].record.cache_read.applicable && sessions[0].record.cache_read.value == 3 &&
            sessions[0].record.cache_read.coverage &&
            sessions[0].record.cache_read.coverage->support == addon::Usage_support::UNKNOWN &&
            sessions[0].record.cache_read.coverage->covered_records == 1 &&
            sessions[0].record.cache_read.coverage->total_records == 2,
            "partial category remains known with incomplete coverage");
        addon::Contract_failure failure;
        require(addon::decode_usage_record(addon::encode_usage_record(sessions[0].record, 2), &failure, 2).has_value(),
            "native aggregate satisfies rich usage contract");
        require(!sessions[0].record.cost_microusd.applicable, "no invented Claude monetary cost");
        auto native = claude::object(R"({"path":"placeholder","size":0,"modified_msecs":0,"records":[{"record_id":"retained-message","session_id":"SessionCase","completed_date":"2026-09-11","total_tokens":30,"source_path":"placeholder","recorded_cwd":"","model":"sonnet","capabilities":{"token_breakdown":{"version":1,"input_tokens":30}}}]})");
        const auto missing = claude::path_string(root / "projects/project/deleted.jsonl");
        native.set("path", addon::Json_value(missing));
        native.find("records")->as_array()[0].set("source_path", addon::Json_value(missing));
        const auto seed_bytes = addon::serialize_json(native) + "\n";
        const auto seed_path = root / "native-seed.jsonl";
        std::ofstream seed(seed_path, std::ios::binary);
        seed << seed_bytes;
        seed.close();
        claude::Usage_cache_input cache;
        cache.native_seed = seed_path;
        cache.seed_sha256_hex = addon::to_hex(addon::Sha256::digest(seed_bytes));
        cache.storage_file = root / "owned-cache.jsonl";
        const auto imported = claude::scan_usage(root, context, cache);
        require(imported.size() == 1 && imported[0].record.total_tokens == 67,
            "native missing-file messages participate before same-session replacement");
        fs::remove_all(root / "projects");
        cache.native_seed.reset();
        cache.cache_required = true;
        const auto retained = claude::scan_usage(root, context, cache);
        require(retained.size() == 1 && retained[0].record.total_tokens == 67,
            "owned cache preserves imported and deleted projects-subtree evidence");
        const auto new_profile = root / "new-profile";
        fs::create_directory(new_profile);
        require(claude::scan_usage(new_profile, context).empty(),
            "new valid profile without projects subtree is empty");
        {
            std::ofstream projects_file(root / "projects", std::ios::binary);
            projects_file << "not a directory";
        }
        bool projects_path_refused = false;
        try { (void)claude::scan_usage(root, context, cache); }
        catch (const std::exception&) { projects_path_refused = true; }
        require(projects_path_refused,
            "existing non-directory projects path rejects retained cache evidence");
        fs::remove(root / "projects");
        fs::remove(*cache.storage_file);
        bool refused = false;
        try { (void)claude::scan_usage(root, context, cache); }
        catch (const std::exception&) { refused = true; }
        require(refused, "required historical cache loss blocks replacement");
    }
    catch (...) {
        fs::remove_all(root);
        throw;
    }
    fs::remove_all(root);
}

void empty_usage_cache_contract()
{
    const auto root = fs::temp_directory_path() / ("claude-empty-cache-" + addon::random_nonce_hex());
    fs::create_directory(root);
    try {
        Context context;
        claude::Usage_cache_input cache;
        cache.native_seed = root / "native-seed.jsonl";
        cache.seed_sha256_hex = addon::to_hex(addon::Sha256::digest(""));
        cache.storage_file = root / "owned-cache.jsonl";
        cache.cache_required = true;
        std::ofstream seed(*cache.native_seed, std::ios::binary);
        seed.close();
        require(bool(seed), "empty native seed fixture written");
        const auto require_missing_evidence = [&] {
            bool refused = false;
            try { (void)claude::scan_usage(root, context, cache); }
            catch (const std::runtime_error& error) {
                refused = std::string_view(error.what()) == "usage_required_cache_unavailable";
            }
            require(refused, "required history needs actual retained message evidence");
        };
        require_missing_evidence();
        require(!fs::exists(*cache.storage_file), "refused empty seed must not publish an owned cache");

        // Fresh empty history remains legal when the host requires no retained evidence.
        cache.cache_required = false;
        require(claude::scan_usage(root, context, cache).empty(),
            "optional empty native seed supports fresh empty history");
        require(fs::is_regular_file(*cache.storage_file), "optional scan persisted its empty cache");
        std::ifstream before(*cache.storage_file, std::ios::binary);
        require(bool(before), "existing cache opens before refusal");
        const std::string accepted_before((std::istreambuf_iterator<char>(before)), std::istreambuf_iterator<char>());
        before.close();
        fs::remove(*cache.native_seed);
        cache.native_seed.reset();
        cache.cache_required = true;
        require_missing_evidence();
        std::ifstream after(*cache.storage_file, std::ios::binary);
        require(bool(after), "existing cache opens after refusal");
        const std::string accepted_after((std::istreambuf_iterator<char>(after)), std::istreambuf_iterator<char>());
        after.close();
        require(accepted_before == accepted_after, "refusal preserves the existing cache bytes");
        cache.cache_required = false;
        require(claude::scan_usage(root, context, cache).empty(),
            "optional header-only cache remains valid on reload");
        fs::remove(*cache.storage_file);
        cache.cache_required = true;
        require_missing_evidence();

        // A nonempty wrapper with no records is still not historical evidence.
        cache.native_seed = root / "native-seed.jsonl";
        auto wrapper = addon::Json_value::object();
        wrapper.set("path", addon::Json_value(claude::path_string(root / "missing.jsonl")));
        wrapper.set("records", addon::Json_value::array());
        const auto wrapped = addon::serialize_json(wrapper) + "\n";
        cache.seed_sha256_hex = addon::to_hex(addon::Sha256::digest(wrapped));
        std::ofstream wrapper_file(*cache.native_seed, std::ios::binary);
        wrapper_file << wrapped;
        wrapper_file.close();
        require(bool(wrapper_file), "zero-record native wrapper fixture written");
        require_missing_evidence();
        require(!fs::exists(*cache.storage_file), "zero-record refusal must not publish a cache");
    }
    catch (...) {
        fs::remove_all(root);
        throw;
    }
    fs::remove_all(root);
}

void retained_message_contract()
{
    for (const bool native_seed : {false, true}) {
        const auto root = fs::temp_directory_path() / ("claude-retained-" + addon::random_nonce_hex());
        const auto source = root / "projects/project/session.jsonl";
        fs::create_directories(source.parent_path());
        fs::create_directory(root / ".git");
        try {
            const auto message = [
                    &root
                ](
                    const char* id,
                    std::uint64_t tokens)
                {
                    auto row = claude::object(R"({"type":"assistant","sessionId":"SessionCase","timestamp":"2026-09-12T12:00:00","message":{"stop_reason":"end_turn","usage":{}}})");
                    row.set("cwd", Json(claude::path_string(root)));
                    row.find("message")->set("id", Json(id));
                    row.find("message")->find("usage")->set("input_tokens", Json(tokens));
                    return row;
                };
            const auto write_rows = [
                    &source
                ](
                    const std::vector<Json>& rows,
                    std::string_view tail)
                {
                    std::ofstream stream(source, std::ios::binary | std::ios::trunc);
                    for (const auto& row : rows) {
                        stream << addon::serialize_json(row) << '\n';
                    }
                    stream << tail;
                    stream.close();
                    require(bool(stream), "transcript fixture written");
                };
            Context context;
            claude::Usage_cache_input cache;
            cache.storage_file = root / "owned-cache.jsonl";
            const auto first  = message("message-A", 25);
            const auto second = message("message-B", 12);
            if (native_seed) {
                Json records = Json::array();
                for (const auto& row : {first, second}) {
                    auto record = claude::object(R"({"session_id":"SessionCase","completed_date":"2026-09-12"})");
                    record.set("record_id", *row.find("message")->find("id"));
                    record.set("total_tokens", *row.find("message")->find("usage")->find("input_tokens"));
                    record.set("source_path", Json(claude::path_string(source)));
                    record.set("recorded_cwd", Json(claude::path_string(root)));
                    records.push_back(std::move(record));
                }
                Json wrapper = Json::object();
                wrapper.set("path", Json(claude::path_string(source)));
                wrapper.set("records", std::move(records));
                const auto bytes = addon::serialize_json(wrapper) + "\n";
                cache.native_seed = root / "native-seed.jsonl";
                cache.seed_sha256_hex = addon::to_hex(addon::Sha256::digest(bytes));
                cache.cache_required = true;
                std::ofstream seed(*cache.native_seed, std::ios::binary);
                seed << bytes;
                seed.close();
                require(bool(seed), "native seed fixture written");
            }
            else {
                write_rows({first, second}, {});
                require(claude::scan_usage(root, context, cache)[0].record.total_tokens == 37,
                    "completed messages establish package retained evidence");
            }

            // usage-scan@2 requires retained message identities even when the
            // surviving source ends before the previously completed message.
            write_rows({first}, "{unfinished");
            auto sessions = claude::scan_usage(root, context, cache);
            require(sessions.size() == 1 && sessions[0].record.total_tokens == 37,
                "partial surviving source preserves both retained completed messages");
            addon::Contract_failure failure;
            const auto decoded = addon::decode_usage_record(
                addon::encode_usage_record(sessions[0].record, 2), &failure, 2);
            require(decoded && decoded->repository_usage && decoded->repository_usage->allocations.size() == 1 &&
                decoded->repository_usage->allocations[0].total_tokens == 37,
                "single real repository allocation satisfies usage-scan@2 codec");
            if (cache.native_seed) {
                fs::remove(*cache.native_seed);
                cache.native_seed.reset();
            }
            cache.cache_required = true;
            auto unfinished = message("message-B", 2);
            unfinished.find("message")->set("stop_reason", Json());
            write_rows({first, unfinished}, {});
            require(claude::scan_usage(root, context, cache)[0].record.total_tokens == 37,
                "in-progress lower message cannot replace completed evidence after cache reload");
            write_rows({message("message-A", 5)}, {});
            require(claude::scan_usage(root, context, cache)[0].record.total_tokens == 17,
                "completed same-ID correction replaces its own value and retains absent message");
            write_rows({message("message-B", 3), message("message-C", 4)}, {});
            require(claude::scan_usage(root, context, cache)[0].record.total_tokens == 12,
                "completed corrections and new message IDs reconcile without a monotonic total");
            write_rows({message("message-B", 3), message("message-B", 6)}, {});
            bool conflicting_usage = false;
            try {
                (void)claude::scan_usage(root, context, cache);
            }
            catch (const std::runtime_error& error) {
                conflicting_usage = std::string_view(error.what()) == "conflicting_message_usage";
            }
            require(conflicting_usage, "fresh duplicate message IDs retain conflicting usage rejection");
            fs::remove(source);
            require(claude::scan_usage(root, context, cache)[0].record.total_tokens == 12,
                "corrected message evidence survives source deletion and cache reload");
        }
        catch (...) {
            fs::remove_all(root);
            throw;
        }
        fs::remove_all(root);
    }
}

void quota_reset_label_contract()
{
    using namespace logonomic::cli_addon;
    const std::vector<std::pair<std::string,std::string>> cases{
        {"7pm (Europe/Berlin)", "7pm (Europe/Berlin)"},
        {"7pm (Europe/Berlin))., ", "7pm (Europe/Berlin)"},
        {"7pm (Europe/Berlin).)", "7pm (Europe/Berlin)"},
        {"Sep 18 (Europe/Berlin)\t", "Sep 18 (Europe/Berlin)"},
        {"7pm., \t", "7pm"},
        {"7pm)", "7pm"},
        {"7pm.)", "7pm"},
        {"7pm.) ,)\t", "7pm"},
        {" ", ""},
    };
    for (const auto& [input, expected] : cases) {
        Json_value event=Json_value::object();
        event.set("type", Json_value("result"));
        event.set("is_error", Json_value(false));
        event.set("result", Json_value("Current session: 12% used, resets " + input +
            "\nCurrent week: 34% used, resets " + input));
        Structured_inspection inspection;
        inspection.final=true; inspection.exit_code=0;
        inspection.stdout_bytes=serialize_json(event);
        const auto observation=claude::inspect_quota(inspection);
        const bool good=expected.empty() ? (!observation.primary && !observation.secondary) :
            (observation.primary && observation.secondary &&
             observation.primary->reset_text.value_or("")==expected &&
             observation.secondary->reset_text.value_or("")==expected);
        std::cout << (good?"PASS":"FAIL") << " reset_label=[" << input << "] expected=[" << expected << "]\n";
        require(good, "ARC-005 quota reset label must preserve balanced parentheses in both windows");
    }
}

void account_name_contract()
{
    // Oracle: accounts@1 names an account with a filesystem profile after the
    // profile's folder through the SDK's account_display_name.
#ifdef _WIN32
    const std::string platform = "windows-x86_64";
    const std::string home     = "C:/Users/Jane Doe";
    const std::string home_env = "USERPROFILE";
#else
    const std::string platform = "linux-x86_64";
    const std::string home     = "/home/jane";
    const std::string home_env = "HOME";
#endif
    const auto discovered_name = [&](const std::string& config_dir) {
        addon::Accounts_launcher_request request;
        request.launcher.script_body = "claude";
        request.launcher.runner_key  = addon::k_runner_direct;
        request.launch_environment.platform_id = platform;
        request.launch_environment.effective_environment = Json::object();
        request.launch_environment.effective_environment.set(home_env, Json(home));
        request.launch_environment.effective_environment.set("CLAUDE_CONFIG_DIR", Json(config_dir));
        request.default_home_fallback   = home;
        request.normalization_directory = home;
        Context discovery;
        discovery.m_request.capability.capability_id = "accounts";
        discovery.m_request.operation = addon::k_accounts_discover_operation;
        discovery.m_request.payload   = addon::encode_accounts_launcher_request(request);
        claude::handle_accounts(discovery);
        addon::Contract_failure failure;
        const auto accounts = addon::decode_accounts_result(discovery.m_payload, &failure);
        return accounts && accounts->size() == 1 ? accounts->front().display_name : std::string{};
    };
    require(discovered_name(home + "/.claude-work/") == ".claude-work",
        "a trailing separator does not hide the profile folder's name");
    const std::string long_home = home + "/.claude-account-for-the-varinomics-nightly-release-builds";
    require(discovered_name(long_home) == addon::account_display_name(long_home),
        "an over-long folder name is fitted by the SDK's account display-name rule");
}

} // namespace

int main()
{
    try {
        launcher_contract();
        launcher_directory_contract();
        lifecycle_contract();
        structured_contract();
        delegated_readiness_contract();
        probe_contract();
#ifdef _WIN32
        probe_profile_contract();
#endif
        quota_reset_label_contract();
        account_name_contract();
        usage_contract();
        empty_usage_cache_contract();
        retained_message_contract();
        std::cout << "Claude native contract checks passed.\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
