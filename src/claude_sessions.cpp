#include "claude_sessions.h"
#include "claude_launcher.h"
#include "claude_accounts.h"
#include "claude_support.h"

#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/launcher_utilities.h>

#include <algorithm>
#include <chrono>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace claude {
namespace {

namespace addon = logonomic::cli_addon;
using Json = addon::Json_value;

std::vector<std::string> string_array(const Json& value)
{
    if (!value.is_array()) {
        throw std::runtime_error("invalid_protocol_state");
    }
    std::vector<std::string> result;
    for (const auto& entry : value.as_array()) {
        if (!entry.is_string() || entry.as_string().empty()) {
            throw std::runtime_error("invalid_protocol_state");
        }
        result.push_back(entry.as_string());
    }
    return result;
}

bool same_strings(std::vector<std::string> left, std::vector<std::string> right)
{
    std::sort(left.begin(), left.end());
    std::sort(right.begin(), right.end());
    return left == right;
}

bool startup_hook(const std::string& type, const std::string& subtype)
{
    return type == "system" &&
        (subtype == "hook_started" || subtype == "hook_progress" || subtype == "hook_response");
}

bool bind_session(const Json& event, std::string& root_session)
{
    const auto id = text(event, "session_id");
    if (id.empty() || (!root_session.empty() && id != root_session)) {
        return false;
    }
    root_session = id;
    return true;
}

bool same_directory(std::string left, std::string right)
{
    left  = path_string(addon::path_from_utf8(left).lexically_normal());
    right = path_string(addon::path_from_utf8(right).lexically_normal());
#ifdef _WIN32
    for (char& ch : left) {
        if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    }
    for (char& ch : right) {
        if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    }
#endif
    return !left.empty() && left == right;
}

bool valid_review_init(const Json& event, const Json& state)
{
    const auto* tools   = event.find("tools");
    const auto* servers = event.find("mcp_servers");
    const auto* expected_tools   = state.find("expected_tools");
    const auto* expected_servers = state.find("expected_mcp_servers");
    if (!tools || !servers || !servers->is_array() || !expected_tools || !expected_servers ||
        !same_directory(text(event, "cwd"), text(state, "working_directory")))
    {
        return false;
    }
    // Settings and hooks remain the user's own. Plugin metadata is not a
    // capability grant: attest the actual tools, allowing user deny rules to
    // narrow the read-only ceiling. Required Task tools are checked separately.
    const auto available = string_array(*tools);
    const auto allowed   = string_array(*expected_tools);
    if (!std::all_of(available.begin(), available.end(), [&allowed](const std::string& tool) {
            return std::find(allowed.begin(), allowed.end(), tool) != allowed.end();
        }))
    {
        return false;
    }
    std::vector<std::string> names;
    for (const auto& server : servers->as_array()) {
        if (!server.is_object() || text(server, "name").empty() || text(server, "status") != "connected")
        {
            return false;
        }
        names.push_back(text(server, "name"));
    }
    return same_strings(std::move(names), string_array(*expected_servers));
}

std::vector<std::string> task_tools(const Json& state)
{
    const auto* tools = state.find("task_tools");
    if (!tools) {
        throw std::runtime_error("invalid_protocol_state");
    }
    return string_array(*tools);
}

// The launcher's options and the user's own settings decide what Claude may
// do; a deny rule for a Task server removes its tools from the session.
bool task_tools_available(const Json& init, const Json& state)
{
    const auto required = task_tools(state);
    if (required.empty()) {
        return true;
    }
    const auto* tools = init.find("tools");
    if (!tools) {
        return false;
    }
    const auto available = string_array(*tools);
    return std::all_of(required.begin(), required.end(), [&available](const std::string& tool) {
        return std::find(available.begin(), available.end(), tool) != available.end();
    });
}

// A rule or hook that refuses one Task tool call is recorded among the root
// result's permission denials.
bool task_tool_denied(const Json& result, const Json& state)
{
    const auto* denials = result.find("permission_denials");
    if (!denials || !denials->is_array()) {
        return false;
    }
    const auto required = task_tools(state);
    return std::any_of(denials->as_array().begin(), denials->as_array().end(), [&required](const Json& denial) {
        return std::find(required.begin(), required.end(), text(denial, "tool_name")) != required.end();
    });
}

addon::Structured_inspect_result failed(std::string reason)
{
    addon::Structured_inspect_result result;
    result.complete = true;
    result.failure_reason = std::move(reason);
    return result;
}

std::string join(const std::vector<std::string>& items)
{
    std::string result;
    for (const auto& item : items) {
        if (!result.empty()) result += ',';
        result += item;
    }
    return result;
}

bool apply_model_selection(
    const addon::Session_launch_context& context,
    const addon::Delegated_model_selection& selection,
    addon::Process_proposal& process)
{
    const auto& source = context.launcher_argv;
    for (std::size_t index = *cli_argument_index(source) + 1; index < source.size(); ++index) {
        const auto& argument = source[index];
        if (argument == "--") {
            return false;
        }
        index += claude_option_value_words(source, index);
    }
    // Native scalar options use the last occurrence. Appending retains every
    // saved argument, including option-looking values owned by other flags.
    if (selection.model) {
        process.argv.push_back("--model=" + *selection.model);
        // An empty CLI fallback overrides saved fallback settings for this
        // process, so overload cannot select a different configured model.
        process.argv.push_back("--fallback-model=");
    }
    if (selection.reasoning_effort) {
        process.argv.push_back("--effort=" + *selection.reasoning_effort);
    }
    return true;
}

std::string control_request(Json& state, const std::string& phase)
{
    const auto id = addon::random_nonce_hex();
    state.set("mcp_phase", Json(phase));
    state.set("mcp_request_id", Json(id));
    Json request = Json::object();
    request.set("subtype", Json(phase == "status" ? "mcp_status" : phase));
    Json frame = Json::object();
    frame.set("type", Json("control_request"));
    frame.set("request_id", Json(id));
    frame.set("request", std::move(request));
    return addon::serialize_json(frame) + '\n';
}

std::optional<bool> required_catalog_ready(const Json& response, const Json& state)
{
    const auto* servers = response.find("mcpServers");
    const auto* required = state.find("required_mcp_servers");
    if (!servers || !servers->is_array() || !required || !required->is_array()) {
        return std::nullopt;
    }
    std::set<std::string> seen;
    for (const auto& server : servers->as_array()) {
        const auto name = text(server, "name");
        if (name.empty() || !seen.insert(name).second) {
            return std::nullopt;
        }
    }
    bool pending = false;
    for (const auto& expected : required->as_array()) {
        const auto name = text(expected, "name");
        const auto server = std::find_if(servers->as_array().begin(), servers->as_array().end(),
            [&name](const Json& entry) { return text(entry, "name") == name; });
        if (server == servers->as_array().end()) {
            return std::nullopt;
        }
        if (text(*server, "status") == "pending") {
            pending = true;
            continue;
        }
        if (text(*server, "status") != "connected") {
            return std::nullopt;
        }
        const auto* tools = server->find("tools");
        const auto* actions = expected.find("actions");
        if (!tools || !tools->is_array() || !actions) {
            return std::nullopt;
        }
        std::set<std::string> names;
        for (const auto& tool : tools->as_array()) {
            const auto action = text(tool, "name");
            if (action.empty() || !names.insert(action).second) {
                return std::nullopt;
            }
        }
        for (const auto& action : string_array(*actions)) {
            if (!names.contains(action)) {
                return std::nullopt;
            }
        }
    }
    return !pending;
}

double readiness_clock_ms()
{
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::optional<std::string> advance_readiness(
    const Json& event, Json& state, addon::Structured_inspect_result& result)
{
    const auto* response = event.find("response");
    if (!response || !response->is_object() || text(*response, "request_id").empty() ||
        text(*response, "request_id") != text(state, "mcp_request_id"))
    {
        return "mcp_control_response_mismatch";
    }
    const auto* payload = response->find("response");
    const auto phase = text(state, "mcp_phase");
    if (text(*response, "subtype") != "success" || !payload || !payload->is_object())
    {
        return phase == "get_settings" ? "model_selection_unavailable" : "mcp_control_unavailable";
    }
    if (phase == "get_settings") {
        const auto* applied = payload->find("applied");
        if (!applied || !applied->is_object()) {
            return "model_selection_unavailable";
        }
        const auto model = text(state, "selected_model");
        const auto effort = text(state, "selected_effort");
        if (!model.empty() && text(*applied, "model") != model) {
            return "model_selection_unavailable";
        }
        if (!effort.empty() && text(*applied, "effort") != effort) {
            return "reasoning_effort_unavailable";
        }
        result.stdin_bytes = control_request(state, "status");
        return std::nullopt;
    }
    if (phase == "status") {
        const auto ready = required_catalog_ready(*payload, state);
        if (!ready) {
            return "required_mcp_unavailable";
        }
        const auto* deadline = state.find("mcp_deadline_ms");
        if (!deadline || !deadline->is_number()) {
            return "invalid_protocol_state";
        }
        if (readiness_clock_ms() >= deadline->as_number()) {
            return "mcp_readiness_incomplete";
        }
        if (!*ready) {
            // Inspection runs in the add-on process. Pace native status queries
            // while discovery is pending without replacing its stdio connection.
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (readiness_clock_ms() >= deadline->as_number()) {
                return "mcp_readiness_incomplete";
            }
            result.stdin_bytes = control_request(state, "status");
            return std::nullopt;
        }
        // Classify the entire buffered native stream before releasing input;
        // an administrative frame may be split after this status response.
        state.set("mcp_phase", Json("verified"));
        state.set("mcp_request_id", Json(""));
        state.erase("mcp_deadline_ms");
        return std::nullopt;
    }
    if (phase != "initialize") {
        return "invalid_protocol_state";
    }
    state.set("mcp_deadline_ms", Json(readiness_clock_ms() + 30000.0));
    if (state.find("selected_model") || state.find("selected_effort")) {
        // Native metadata resolves aliases without constraining custom model
        // IDs to the menu. get_settings reports the applied value after policy
        // caps and model support, which --effort alone does not attest.
        const auto model = text(state, "selected_model");
        const auto* models = payload->find("models");
        if (!model.empty() && models && models->is_array()) {
            for (const auto& entry : models->as_array()) {
                if (text(entry, "value") == model && !text(entry, "resolvedModel").empty()) {
                    state.set("selected_model", Json(text(entry, "resolvedModel")));
                    break;
                }
            }
        }
        result.stdin_bytes = control_request(state, "get_settings");
    }
    else {
        result.stdin_bytes = control_request(state, "status");
    }
    return std::nullopt;
}

std::optional<std::string> verify_model_selection(
    const addon::Session_launch_context& context,
    const addon::Delegated_model_selection& selection,
    const std::filesystem::path& package_root,
    addon::Invocation_context& invocation,
    std::string& selected_model)
{
    // A native settings reply includes raw settings and may contain secrets.
    // Keep it inside a private, prompt-free probe, outside host task stdout and
    // its failure diagnostics. The actual task retains its normal MCP startup.
    for (std::size_t index = *cli_argument_index(context.launcher_argv) + 1;
         index < context.launcher_argv.size(); ++index)
    {
        const auto& argument = context.launcher_argv[index];
        if (argument == "--mcp-config" || argument.starts_with("--mcp-config=")) {
            return "model_selection_unavailable";
        }
        index += claude_option_value_words(context.launcher_argv, index);
    }
    auto process = cli_process(context, true);
    apply_account(context, process);
    if (!apply_model_selection(context, selection, process)) {
        return "model_selection_unavailable";
    }
    process.argv.insert(process.argv.end(), {"-p", "--input-format", "stream-json", "--output-format",
        "stream-json", "--verbose", "--no-session-persistence", "--strict-mcp-config", "--mcp-config",
        "{\"mcpServers\":{}}", "--tools", ""});
    auto environment = addon::environment_pairs(context.launch_environment->effective_environment);
    for (const auto& [name, value] : process.environment) {
        addon::set_environment(environment, name, value);
    }
    const auto program = addon::resolve_program(context.launcher_argv.front(), environment,
        context.working_directory, addon::Launcher_whitespace::ASCII);
    if (!program) {
        return "model_selection_unavailable";
    }
    std::vector<std::string> argv{path_string(*program)};
    if (!addon::native_image(*program)) {
#ifdef _WIN32
        argv.insert(argv.begin(), path_string(package_root / "helpers/claude_launch_bridge.exe"));
#else
        argv.insert(argv.begin(), path_string(package_root / "helpers/claude_launch_bridge"));
#endif
    }
    argv.insert(argv.end(), process.argv.begin(), process.argv.end());
    Json state = Json::object();
    if (selection.model) state.set("selected_model", Json(*selection.model));
    if (selection.reasoning_effort) state.set("selected_effort", Json(*selection.reasoning_effort));
    addon::Probe_exchange exchange;
    exchange.initial_input = control_request(state, "initialize");
    std::size_t consumed = 0;
    bool pending_before_input = false;
    std::string root_session;
    std::string refusal = "model_selection_unavailable";
    exchange.on_output = [&](std::string_view output) -> std::optional<std::string> {
        std::string input;
        try {
            for (std::size_t end; (end = output.find('\n', consumed)) != std::string_view::npos;) {
                const auto line = output.substr(consumed, end - consumed);
                consumed = end + 1;
                if (line.find_first_not_of(" \t\r") == std::string_view::npos) continue;
                const auto event = object(line);
                if (startup_hook(text(event, "type"), text(event, "subtype"))) {
                    if (!bind_session(event, root_session)) return std::nullopt;
                    pending_before_input = false;
                    continue;
                }
                // Hooks can cross a control write; a reply cannot acknowledge
                // input this callback has not returned to the probe yet.
                if (!input.empty() || pending_before_input || exchange.complete) return std::nullopt;
                if (text(event, "type") != "control_response") return std::nullopt;
                addon::Structured_inspect_result next;
                if (const auto failure = advance_readiness(event, state, next)) {
                    refusal = *failure == "reasoning_effort_unavailable"
                        ? *failure : "model_selection_unavailable";
                    return std::nullopt;
                }
                if (text(state, "mcp_phase") == "status") {
                    selected_model = text(state, "selected_model");
                    exchange.complete = true;
                }
                else
                if (next.stdin_bytes) {
                    input = std::move(*next.stdin_bytes);
                }
            }
            if (!input.empty() && output.substr(consumed).find_first_not_of(" \t\r\n") != std::string_view::npos) {
                pending_before_input = true;
            }
        }
        catch (const std::exception&) {
            return std::nullopt;
        }
        return input;
    };
    if (!addon::probe(argv, environment, context.working_directory, 10000, invocation, &exchange) ||
        !exchange.complete)
    {
        return refusal;
    }
    return std::nullopt;
}

addon::Structured_prepare_result prepare(
    const addon::Session_launch_context& context,
    addon::Structured_purpose purpose,
    const std::string& prompt,
    const std::string& mcp_path,
    const std::vector<std::string>& delegated_tools,
    const std::vector<std::string>& server_names,
    const Json& required_servers)
{
    if (prompt.empty()) {
        throw std::runtime_error("unsupported_launcher");
    }
    const bool review = purpose == addon::Structured_purpose::READ_ONLY_REVIEW;
    const auto command = recognize_launcher(context.launcher.script_body, context.launcher.runner_key);
    if (!command || !direct_implementation_arguments(context)) {
        throw std::runtime_error("unsupported_launcher");
    }
    if (review) {
        // Inspect the same normalized argv that will execute. Quoted pieces
        // of option names must not evade the review ceiling, and '--' would
        // turn our appended restrictions into positional input.
        for (std::size_t index = *cli_argument_index(context.launcher_argv) + 1;
             index < context.launcher_argv.size(); ++index)
        {
            const auto& argument = context.launcher_argv[index];
            const auto name = argument.substr(0, argument.find('='));
            if (name == "--tools" || name == "--mcp-config" || name == "--") {
                throw std::runtime_error("review_launcher_conflict");
            }
            index += claude_option_value_words(context.launcher_argv, index);
        }
    }
    addon::Structured_prepare_result result;
    result.process = cli_process(context, true);
    apply_account(context, result.process);
    auto& args = result.process.argv;
    args.insert(args.end(), {"-p", "--output-format", "stream-json", "--verbose", "--no-session-persistence"});
    std::vector<std::string> review_tools{"Read", "Glob", "Grep"};
    review_tools.insert(review_tools.end(), delegated_tools.begin(), delegated_tools.end());
    if (review) {
        // A tool ceiling enforces the review purpose without overriding the
        // launcher's permission mode or disabling user permission hooks.
        // Plan mode itself denies Task tools in current Claude Code.
        args.insert(args.end(), {"--strict-mcp-config", "--mcp-config", mcp_path, "--tools", join(review_tools)});
    }
    else {
        // An implementation keeps the launcher's own options and the user's
        // settings, which alone decide what Claude may do; headless Claude
        // denies what they do not grant. The package pre-approves only this
        // task's own Task tools.
        if (!server_names.empty()) {
            args.insert(args.end(), {"--mcp-config", mcp_path});
        }
    }
    if (!delegated_tools.empty()) {
        args.insert(args.end(), {"--allowedTools", join(delegated_tools)});
    }
    result.process.stdin_behavior = addon::Stdin_behavior::PAYLOAD;
    result.process.stdin_payload  = prompt;
    result.profile_storage = context.profile_storage;
    result.account_context = context.account_context;
    result.inspection.incremental = true;
    Json state = Json::object();
    state.set("initialized", Json(false));
    state.set("pending_base64", Json(""));
    state.set("result_base64", Json(""));
    state.set("root_session_id", Json(""));
    state.set("working_directory", Json(context.working_directory));
    if (review) {
        state.set("expected_tools", strings(review_tools));
    }
    state.set("task_tools", strings(delegated_tools));
    state.set("expected_mcp_servers", strings(server_names));
    if (!server_names.empty()) {
        args.insert(args.end(), {"--input-format", "stream-json"});
        state.set("required_mcp_servers", required_servers);
        state.set("prompt_base64", Json(addon::encode_base64(prompt)));
        result.process.stdin_payload = control_request(state, "initialize");
        result.inspection.keep_stdin_open = true;
    }
    result.inspection.protocol_state = addon::serialize_json(state);
    return result;
}

} // namespace

addon::Structured_inspect_result inspect_session(
    addon::Structured_purpose purpose, const addon::Structured_inspection& inspection)
{
    try {
        auto parsed = addon::parse_exchange_protocol_state(inspection.protocol_state);
        if (!parsed || !parsed->is_object()) return failed("invalid_structured_output");
        Json state = std::move(*parsed);
        const auto* initialized_value = state.find("initialized");
        auto pending_value = addon::decode_base64(text(state, "pending_base64"));
        auto result_value  = addon::decode_base64(text(state, "result_base64"));
        if (!initialized_value || !initialized_value->is_boolean() || !pending_value || !result_value) {
            return failed("invalid_protocol_state");
        }
        bool initialized = initialized_value->as_boolean();
        std::string root_session = text(state, "root_session_id");
        std::string pending = std::move(*pending_value);
        pending += inspection.stdout_bytes;
        bool pending_before_input = flag(state, "pending_before_input");
        std::string answer = std::move(*result_value);
        const bool review = purpose == addon::Structured_purpose::READ_ONLY_REVIEW;
        const auto initial_phase = text(state, "mcp_phase");
        if (!initial_phase.empty() && initial_phase != "released" && inspection.final) {
            return failed(inspection.timed_out ? "process_timed_out" : "mcp_readiness_incomplete");
        }
        if ((initialized && root_session.empty()) || (!answer.empty() && !initialized)) {
            return failed("invalid_protocol_state");
        }
        addon::Structured_inspect_result result;
        auto record = [&result](addon::Presentation_event_kind kind) {
            result.presentation_state = addon::Presentation_state::AVAILABLE;
            if (!result.presentation.empty() && result.presentation.back().kind == kind) {
                ++result.presentation.back().count;
            }
            else {
                result.presentation.push_back({kind, 1});
            }
        };
        std::size_t consumed = 0;
        for (std::size_t end; (end = pending.find('\n', consumed)) != std::string::npos;) {
            const auto line = pending.substr(consumed, end - consumed);
            consumed = end + 1;
            if (line.find_first_not_of(" \r\t") == std::string::npos) continue;
            const auto event = object(line);
            const auto type = text(event, "type");
            const auto subtype = text(event, "subtype");
            if (type.empty() || (review && !answer.empty())) return failed("invalid_structured_output");
            const bool administrative = ((review || !initialized) && startup_hook(type, subtype)) ||
                (review && type == "system" && (subtype == "commands_changed" || subtype == "informational"));
            if (administrative) {
                if (!bind_session(event, root_session)) {
                    return failed("session_provenance_mismatch");
                }
                pending_before_input = false;
                record(addon::Presentation_event_kind::REASONING);
                continue;
            }
            // A reply cannot acknowledge input that this inspection has not
            // returned to the host yet. Only administration may cross it.
            if (result.stdin_bytes || pending_before_input) {
                return failed("unexpected_preprompt_output");
            }
            pending_before_input = false;
            const auto phase = text(state, "mcp_phase");
            const bool session_fallback = subtype == "model_fallback" || subtype == "model_consent_fallback" ||
                (subtype == "model_refusal_fallback" && (!event.find("scope") || text(event, "scope") == "session"));
            if (state.find("verified_model") && type == "system" && session_fallback &&
                (root_session.empty() || text(event, "session_id") == root_session))
            {
                return failed("model_selection_unavailable");
            }
            if (!phase.empty() && phase != "released") {
                if (type != "control_response") {
                    return failed("unexpected_preprompt_output");
                }
                if (const auto failure = advance_readiness(event, state, result)) {
                    return failed(*failure);
                }
                continue;
            }
            if (!phase.empty() && type == "control_response") {
                return failed("mcp_control_response_mismatch");
            }
            if (type == "system" && subtype == "init") {
                const auto model = text(state, "verified_model");
                if (!model.empty() && text(event, "model") != model) {
                    return failed("model_selection_unavailable");
                }
                const auto id = text(event, "session_id");
                if (id.empty() || (!root_session.empty() && id != root_session)) {
                    return failed("session_provenance_mismatch");
                }
                if (!initialized && !task_tools_available(event, state)) {
                    return failed("task_tools_not_permitted");
                }
                if (review) {
                    if (initialized || !valid_review_init(event, state)) return failed("review_surface_mismatch");
                    record(addon::Presentation_event_kind::REVIEW_INITIALIZED);
                }
                else {
                    if (!initialized) record(addon::Presentation_event_kind::REASONING);
                }
                root_session = id;
                initialized = true;
                continue;
            }
            if (!review && !initialized) {
                return failed("session_provenance_mismatch");
            }
            if (type == "result") {
                if (!initialized) return failed("result_before_initialization");
                const auto id = text(event, "session_id");
                const auto* parent = event.find("parent_tool_use_id");
                const auto* origin = event.find("origin");
                if (id.empty() ||
                    (parent && !parent->is_null() && (!parent->is_string() || parent->as_string().empty())) ||
                    (origin && !origin->is_null() && (!origin->is_object() || text(*origin, "kind").empty())))
                {
                    return failed("invalid_result_provenance");
                }
                if (id != root_session || (parent && !parent->is_null())) {
                    continue;
                }
                if (task_tool_denied(event, state)) {
                    return failed("task_tools_not_permitted");
                }
                const auto* is_error = event.find("is_error");
                const auto value = text(event, "result");
                if (!answer.empty() || value.empty() || value.size() > 1024 * 1024 ||
                    !is_error || !is_error->is_boolean() || is_error->as_boolean() || subtype != "success")
                {
                    return failed("invalid_final_result");
                }
                answer = value;
                record(addon::Presentation_event_kind::RESULT_AUTHENTICATED);
                continue;
            }
            if (type == "system") {
                if (review && (subtype != "thinking_tokens" || !initialized)) return failed("unexpected_system_event");
                record(addon::Presentation_event_kind::REASONING);
            }
            else
            if (type == "assistant") record(addon::Presentation_event_kind::ANALYSIS_UPDATE);
            else
            if (type == "user") record(addon::Presentation_event_kind::TOOL_RESPONSE);
            else
            if (type == "rate_limit_event") record(addon::Presentation_event_kind::RATE_LIMIT_STATUS);
            else
            if (review) return failed("unsupported_structured_event");
        }
        pending.erase(0, consumed);
        if (result.stdin_bytes && pending.find_first_not_of(" \t\r\n") != std::string::npos) {
            pending_before_input = true;
        }
        if (text(state, "mcp_phase") == "verified" && pending.find_first_not_of(" \t\r\n") == std::string::npos) {
            auto prompt = addon::decode_base64(text(state, "prompt_base64"));
            if (!prompt || prompt->empty()) {
                return failed("invalid_protocol_state");
            }
            Json message = Json::object();
            message.set("role", Json("user"));
            message.set("content", Json(std::move(*prompt)));
            Json frame = Json::object();
            frame.set("type", Json("user"));
            frame.set("session_id", Json(""));
            frame.set("parent_tool_use_id", Json());
            frame.set("message", std::move(message));
            result.stdin_bytes = addon::serialize_json(frame) + '\n';
            result.close_stdin = true;
            state.set("mcp_phase", Json("released"));
            state.set("prompt_base64", Json(""));
        }
        if (review && !answer.empty() && pending.find_first_not_of(" \t\r\n") != std::string::npos) {
            return failed("output_after_result");
        }
        if (inspection.final) {
            if (inspection.timed_out) return failed("process_timed_out");
            if (inspection.exit_code != 0) return failed("process_failed");
            if (!initialized || answer.empty() || pending.find_first_not_of(" \t\r\n") != std::string::npos) {
                return failed("unauthenticated_final_result");
            }
        }
        result.complete = inspection.final;
        if (result.complete) {
            result.result_bytes = std::move(answer);
        }
        else {
            state.set("initialized", Json(initialized));
            state.set("pending_base64", Json(addon::encode_base64(pending)));
            state.set("pending_before_input", Json(pending_before_input));
            state.set("result_base64", Json(addon::encode_base64(answer)));
            state.set("root_session_id", Json(root_session));
            result.protocol_state = addon::serialize_json(state);
            if (!addon::exchange_protocol_state_valid(result.protocol_state)) return failed("protocol_state_limit");
        }
        return result;
    }
    catch (const std::exception&) {
        return failed("invalid_structured_output");
    }
}

void handle_sessions(addon::Invocation_context& invocation, const std::filesystem::path& package_root)
{
    const auto& request = invocation.request();
    addon::Contract_failure failure;
    if (request.operation == "inspect" && request.capability.capability_id == "structured-session") {
        const auto inspection = addon::decode_structured_inspect_request(request.payload, &failure);
        if (!inspection) {
            invocation.fail(addon::Result_status::FAILED, failure.reason_code);
            return;
        }
        invocation.succeed(addon::encode_structured_inspect_result(inspect_session(inspection->purpose, inspection->inspection)));
        return;
    }
    if (request.operation != "prepare") {
        invocation.reject_unknown_operation();
        return;
    }
    if (request.capability.capability_id == "structured-session") {
        const auto input = addon::decode_structured_prepare_request(request.payload, &failure);
        if (!input) {
            invocation.fail(addon::Result_status::FAILED, failure.reason_code);
            return;
        }
        auto result = prepare(input->context, input->purpose, input->prompt,
            path_string(package_root / "assets/empty_mcp.json"), {}, {}, Json::array());
        result.process.assets.push_back("assets/empty_mcp.json");
        invocation.succeed(addon::encode_structured_prepare_result(result));
        return;
    }
    const auto input = addon::decode_delegated_prepare_request(request.payload, &failure);
    if (!input) {
        invocation.fail(addon::Result_status::FAILED, failure.reason_code);
        return;
    }
    if (!input->control.connection) {
        invocation.fail(addon::Result_status::UNAVAILABLE, "task_server_connection_unavailable");
        return;
    }
    const auto& connection = *input->control.connection;
    Json servers = Json::object();
    std::vector<std::string> tools;
    std::vector<std::string> names;
    Json required_servers = Json::array();
    addon::Delegated_prepare_result result;
    for (const auto& server : input->control.servers) {
        const std::string name = "logonomic-task-" + input->control.task_id + '-' + server.server_id;
        names.push_back(name);
        Json required = Json::object();
        required.set("name", Json(name));
        required.set("actions", strings(server.actions));
        required_servers.push_back(std::move(required));
        for (const auto& action : server.actions) tools.push_back("mcp__" + name + "__" + action);
        Json config = Json::object();
        config.set("type", Json("stdio"));
        config.set("command", Json(connection.relay_program));
        config.set("args", strings({"--descriptor", connection.descriptor_path, "--server", server.server_id}));
        config.set("env", Json::object());
        servers.set(name, std::move(config));
        result.acknowledgment.server_ids.push_back(server.server_id);
    }
    result.acknowledgment.connection_version = addon::k_delegated_connection_version;
    Json config = Json::object();
    config.set("mcpServers", std::move(servers));
    const std::string relative = "mcp/" + input->control.task_id + ".json";
    result.private_artifacts.push_back({relative, addon::serialize_json(config)});
    auto prepared = prepare(input->context, input->purpose, input->prompt,
        path_string(addon::path_from_utf8(input->control.private_artifact_root) / relative), tools, names, required_servers);
    if (input->model_selection && !apply_model_selection(input->context, *input->model_selection, prepared.process)) {
        invocation.fail(addon::Result_status::UNAVAILABLE, "model_selection_unavailable");
        return;
    }
    if (input->model_selection) {
        std::string selected_model;
        if (const auto failure = verify_model_selection(input->context, *input->model_selection,
                package_root, invocation, selected_model))
        {
            invocation.fail(invocation.cancel_requested() ? addon::Result_status::CANCELLED : addon::Result_status::UNAVAILABLE,
                invocation.cancel_requested() ? "cancelled" : *failure);
            return;
        }
        auto state = object(prepared.inspection.protocol_state);
        if (input->model_selection->model) {
            state.set("verified_model", Json(std::move(selected_model)));
        }
        prepared.inspection.protocol_state = addon::serialize_json(state);
    }
    result.process         = std::move(prepared.process);
    result.inspection      = std::move(prepared.inspection);
    result.profile_storage = input->context.profile_storage;
    result.account_context = input->context.account_context;
    result.model_selection = input->model_selection;
    invocation.succeed(addon::encode_delegated_prepare_result(result));
}

} // namespace claude
