#include "claude_helper.h"
#include "claude_support.h"
#include "claude_wake.h"

#include <logonomic_cli_addon/helper_client.h>
#include <logonomic_cli_addon/execution_context.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/json.h>
#include <logonomic_cli_addon/lifecycle_events.h>
#include <logonomic_cli_addon/limits.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <poll.h>
#include <unistd.h>
#endif

#include <cstdio>
#include <algorithm>
#include <chrono>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace claude {
namespace {

namespace addon = logonomic::cli_addon;

// Native per-hook budgets bound the whole event sequence. Every SDK call sees
// the same deadline and cancellation predicate, and confirms wrapper exit.

int refuse()
{
    std::fputs("Claude event helper: invalid request or unavailable session.\n", stderr);
    return addon::k_helper_exit_invalid_arguments;
}

bool parent_pipe_closed()
{
#if defined(_WIN32)
    // The plugin owns the write end. EOF is its cancellation signal; it does
    // not terminate this process before the SDK can reap the wrapper it owns.
    DWORD available = 0;
    return !PeekNamedPipe(GetStdHandle(STD_INPUT_HANDLE), nullptr, 0, nullptr, &available, nullptr) || available != 0;
#else
    pollfd input{STDIN_FILENO, POLLIN, 0};
    const int ready = poll(&input, 1, 0);
    return ready > 0 && (input.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0;
#endif
}

std::optional<addon::Json_value> read_event(bool watch_parent)
{
    std::string bytes;
    while (true) {
        const int next = std::fgetc(stdin);
        if (next == EOF || (watch_parent && next == '\n')) {
            if (watch_parent && next == EOF) {
                return std::nullopt;
            }
            break;
        }
        if (bytes.size() >= 4 * 1024 * 1024) {
            return std::nullopt;
        }
        bytes.push_back(static_cast<char>(next));
    }
    if (std::ferror(stdin)) {
        return std::nullopt;
    }
    addon::Json_parse_error failure;
    addon::Json_parse_limits limits;
    limits.max_string_bytes = 4 * 1024 * 1024;
    auto event = addon::parse_json(bytes, limits, &failure);
    if (!event || !event->is_object()) {
        return std::nullopt;
    }
    const auto* name = event->find("hook_event_name");
    if (!name || !name->is_string() || name->as_string().empty()) {
        return std::nullopt;
    }
    // Keep native correlation and bounded final-paragraph evidence for the
    // local conversational heuristic; prompts and tool results stay here.
    auto sanitized = addon::Json_value::object();
    for (const auto key : {"hook_event_name", "session_id", "source", "reason", "agent_id", "agent_type",
            "prompt_id", "tool_name", "tool_use_id", "error"})
    {
        if (const auto* value = event->find(key)) sanitized.set(key, *value);
    }
    if (name->as_string() == "SessionStart" && !event->find("agent_id")) {
        // Only the native session boundary supplies this snapshot. Tool input,
        // transcript paths and the original project directory are not its cwd.
        if (const auto* cwd = event->find("cwd")) {
            sanitized.set("cwd", cwd->is_string() && addon::is_execution_local_path(cwd->as_string())
                ? *cwd : addon::Json_value());
        }
    }
    if (const auto* context = event->find("context_state")) {
        auto bounded = addon::Json_value::object();
        for (const auto key : {"used_tokens", "context_window", "usage_unavailable"}) {
            const auto* value = context->find(key);
            if (value) {
                bounded.set(key, value->is_number() || value->is_boolean() || value->is_null()
                    ? *value : addon::Json_value());
            }
        }
        sanitized.set("context_state", std::move(bounded));
    }
    if (name->as_string() == "Stop" && !event->find("agent_id")) {
        const auto* reply = event->find("last_assistant_message");
        if (reply && reply->is_string()) {
            auto evidence = addon::assistant_reply_evidence(reply->as_string());
            if (!evidence.is_null()) {
                sanitized.set("assistant_reply_evidence", std::move(evidence));
            }
        }
    }
    std::string prompt;
    for (const auto key : {"prompt", "user_prompt", "message"}) {
        const auto* value = event->find(key);
        if (value && value->is_string()) {
            prompt = value->as_string();
            break;
        }
    }
    for (char& ch : prompt) {
        if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    }
    const auto contains_any = [&prompt](std::initializer_list<std::string_view> phrases) {
        return std::any_of(phrases.begin(), phrases.end(), [&prompt](auto phrase) {
            return prompt.find(phrase) != std::string::npos;
        });
    };
    const bool changed_files = contains_any({"show", "open", "view", "display", "bring up", "pull up"}) &&
        contains_any({"changed file", "modified file", "edited file", "my changes", "your changes",
            "changes you made", "files you changed", "files you modified", "files you edited"});
    sanitized.set("changed_files_requested", addon::Json_value(changed_files));
    return sanitized;
}

} // namespace

int run_helper(int argc, char** argv)
{
    if (argc == 2 && std::string_view(argv[1]) == "wake-owner") {
        return anchor_wake_owner() ? 0 : addon::k_helper_exit_invalid_arguments;
    }
#if defined(_WIN32)
    if (_setmode(_fileno(stdin), _O_BINARY) == -1 ||
        _setmode(_fileno(stdout), _O_BINARY) == -1)
    {
        return refuse();
    }
#endif
    const bool watch_parent = argc == 3 && std::string_view(argv[2]) == "--watch-parent";
    if ((argc != 2 && !watch_parent) || std::string_view(argv[1]) != "event") {
        return refuse();
    }
    const auto event = read_event(watch_parent);
    addon::Contract_failure failure;
    auto environment = addon::read_helper_environment(&failure);
    if (!event || !environment) {
        return refuse();
    }
    const auto key   = addon::random_nonce_hex();
    const auto nonce = addon::random_nonce_hex();
    const auto& name = event->find("hook_event_name")->as_string();
    const int budget_ms = name == "SessionEnd" ? 4000 :
        name == "SessionStart" || name == "UserPromptSubmit" ? 17000 : 12000;
    // Reserve normal cancellation/reaping time before the native relay stops
    // accepting stdout. OS-delayed termination still retains confirmed custody;
    // this reserve is not a hard bound on process exit.
    const int completion_reserve = name == "SessionStart" || name == "UserPromptSubmit"
        ? addon::limits::k_reap_deadline_ms + 250 : 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms - completion_reserve);
    const auto cancelled = [watch_parent, deadline] {
        return std::chrono::steady_clock::now() >= deadline || (watch_parent && parent_pipe_closed());
    };
    addon::Helper_client client(std::move(*environment), cancelled);
    bool context_emitted = false;
    const bool root_context = !event->find("agent_id");
    const auto emit_context = [&]() {
        if (!root_context || cancelled()) return true;
        const auto* changed = event->find("changed_files_requested");
        const auto instruction = client.request_title_instruction("claude", name,
            name == "UserPromptSubmit" && changed && changed->is_boolean() && changed->as_boolean(),
            addon::Model_context_delivery{});
        if (!instruction.completed || !instruction.accepted || instruction.instruction.empty()) return true;
        auto hook = addon::Json_value::object();
        hook.set("hookEventName", addon::Json_value(name));
        hook.set("additionalContext", addon::Json_value(instruction.instruction));
        auto result = addon::Json_value::object();
        result.set("hookSpecificOutput", std::move(hook));
        const std::string bytes = addon::serialize_json(result) + "\n";
        if (std::fwrite(bytes.data(), 1, bytes.size(), stdout) != bytes.size() || std::fflush(stdout) != 0) return false;
        context_emitted = true;
        return true;
    };
    const auto tool = text(*event, "tool_name");
    if (name == "PreToolUse" && tool != "AskUserQuestion" && tool != "ExitPlanMode") {
        // Context-only checkpoints must not create native attention transitions.
        return emit_context() ? addon::k_helper_exit_committed : addon::k_helper_exit_transport_failure;
    }
    // An active root prompt can carry host-admitted instructions even when
    // its independent lifecycle evidence lacks native turn correlation.
    if (name == "UserPromptSubmit" && !emit_context()) return addon::k_helper_exit_transport_failure;
    auto outcome = client.deliver_provider_event(event->find("hook_event_name")->as_string(), *event, key, nonce);
    if (!outcome.committed && !cancelled() &&
        (outcome.exit_code == addon::k_helper_exit_transport_failure ||
         outcome.exit_code == addon::k_helper_exit_pending))
    {
        // A lost response must replay the original evidence and transition,
        // never mint a second lifecycle transition.
        outcome = client.deliver_provider_event(event->find("hook_event_name")->as_string(), *event, key, nonce);
    }
    if (!outcome.committed) {
        std::fputs("Claude event helper: delivery did not commit.\n", stderr);
        return context_emitted ? addon::k_helper_exit_committed : outcome.exit_code;
    }
    // SessionStart must establish the new native identity before requesting
    // instructions; it is not the already-active prompt case above.
    if (name == "SessionStart" && outcome.accepted) {
        // Compaction carries context for the existing ledger transition.
        if (text(*event, "source") != "compact") register_wake(*event, nonce);
        if (!emit_context()) return addon::k_helper_exit_transport_failure;
    }
    if (name == "SessionEnd" && outcome.accepted) {
        unregister_wake(*event);
    }
    // Consumption releases the host's retained private response. A failed
    // acknowledgement does not undo an already committed delivery.
    (void)client.acknowledge_provider_event(key);
    return context_emitted || outcome.accepted ? addon::k_helper_exit_committed : addon::k_helper_exit_transport_failure;
}

} // namespace claude

int main(int argc, char** argv)
{
    return claude::run_helper(argc, argv);
}
