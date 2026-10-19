#include "claude_sessions.h"

#include <logonomic_cli_addon/json.h>
#include <logonomic_cli_addon/session_contracts.h>

#include <array>
#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace addon = logonomic::cli_addon;

void require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

std::string initial_state()
{
    return R"({"initialized":true,"pending_base64":"","result_base64":"","root_session_id":"session","working_directory":"/repo","expected_tools":[],"task_tools":[],"expected_mcp_servers":[]})";
}

addon::Structured_inspect_result inspect(std::string state, std::string bytes, bool final = false,
    int exit_code = 0)
{
    addon::Structured_inspection request;
    request.protocol_state = std::move(state);
    request.stdout_bytes   = std::move(bytes);
    request.final          = final;
    request.exit_code      = exit_code;
    return claude::inspect_session(addon::Structured_purpose::IMPLEMENTATION, request);
}

std::string result_record(std::string_view answer)
{
    return R"({"type":"result","subtype":"success","session_id":"session","is_error":false,"result":)" +
        addon::serialize_json(addon::Json_value(std::string(answer))) + "}\n";
}

void pending_record_survives_inspections()
{
    const std::size_t text_bytes = 1024 * 1024 - 1024;
    const auto bytes = R"({"type":"assistant","session_id":"session","message":{"content":[{"type":"text","text":")" +
        std::string(text_bytes, 'x') + "\"}]}}\n" + result_record("done");
    const auto whole = inspect(initial_state(), bytes, true);
    require(whole.complete && !whole.failure_reason && whole.result_bytes == "done",
        "whole native record must produce the control answer");
    for (const auto split : std::array<std::size_t, 6>{1, 768 * 1024 - 1, 768 * 1024,
            768 * 1024 + 1, 800 * 1024, text_bytes})
    {
        const auto first = inspect(initial_state(), bytes.substr(0, split));
        require(!first.complete && !first.failure_reason &&
            addon::exchange_protocol_state_valid(first.protocol_state), "prefix must produce a valid continuation");
        const auto idle = inspect(first.protocol_state, "");
        require(!idle.complete && !idle.failure_reason && !idle.result_bytes,
            "an empty inspection must preserve the accepted partial record");
        const auto last = inspect(idle.protocol_state, bytes.substr(split), true);
        require(last.complete && !last.failure_reason && last.result_bytes == whole.result_bytes,
            "chunk boundaries must not change a valid native result");
    }
}

void retained_result_survives_inspections()
{
    const std::string answer(800 * 1024, 'x');
    const auto first = inspect(initial_state(), result_record(answer));
    require(!first.complete && !first.failure_reason &&
        addon::exchange_protocol_state_valid(first.protocol_state), "large result must fit the continuation contract");
    const auto idle = inspect(first.protocol_state, "");
    require(!idle.complete && !idle.failure_reason, "large retained result must survive an empty inspection");
    const auto last = inspect(idle.protocol_state, "", true);
    require(last.complete && !last.failure_reason && last.result_bytes == answer,
        "clean process exit must return the complete retained answer");
    require(inspect(first.protocol_state, "", true, 7).failure_reason == "process_failed",
        "retaining a large answer must not hide an unsuccessful process exit");
}

} // namespace

int main()
{
    int failures = 0;
    const std::array<std::pair<const char*, void (*)()>, 2> tests{{
        {"pending record", pending_record_survives_inspections},
        {"retained result", retained_result_survives_inspections},
    }};
    for (const auto& [name, test] : tests) {
        try {
            test();
        }
        catch (const std::exception& error) {
            ++failures;
            std::cerr << name << ": " << error.what() << '\n';
        }
    }
    std::cout << "claude_continuation_tests cases=" << tests.size() << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
