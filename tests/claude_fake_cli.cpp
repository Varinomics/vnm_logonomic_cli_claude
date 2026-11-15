// Stands in for the Claude CLI in delivery tests: appends the arguments it
// received to the file named by CLAUDE_TEST_ARGV_CAPTURE, one JSON line per run.
// On Windows it records both standard parses of its command line, the C
// runtime's argv and CommandLineToArgvW's, because the real CLI's parser is not
// observable here and either one must recover every argument unchanged.
#include <logonomic_cli_addon/json.h>
#include <logonomic_cli_addon/package_source.h>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace addon = logonomic::cli_addon;

std::string hook_frame(std::string_view subtype, std::string_view session = "selection-session")
{
    auto frame = addon::Json_value::object();
    frame.set("type", addon::Json_value("system"));
    frame.set("subtype", addon::Json_value(std::string(subtype)));
    frame.set("session_id", addon::Json_value(std::string(session)));
    return addon::serialize_json(frame) + '\n';
}

std::optional<int> selection_probe(const std::vector<std::string>& arguments)
{
    const char* mode = std::getenv("CLAUDE_TEST_SELECTION_PROBE");
    if (!mode) return std::nullopt;
    const auto mcp = std::find(arguments.begin(), arguments.end(), "--mcp-config");
    if (std::find(arguments.begin(), arguments.end(), "--strict-mcp-config") == arguments.end() ||
        mcp == arguments.end() || std::next(mcp) == arguments.end() || *std::next(mcp) != "{\"mcpServers\":{}}")
    {
        return 6;
    }
    std::string model = "saved-model";
    std::string effort = "saved-effort";
    for (const auto& argument : arguments) {
        if (argument.starts_with("--model=")) model = argument.substr(8);
        if (argument.starts_with("--effort=")) effort = argument.substr(9);
    }
    if (model == "opus") model = "claude-native-opus";
    if (std::string_view(mode) == "clamped") effort = "policy-capped-effort";
    if (std::string_view(mode) == "substituted") model = "substituted-model";
    const char* expected = std::getenv("CLAUDE_TEST_EXPECTED_PROFILE");
    const char* selected = std::getenv("CLAUDE_CONFIG_DIR");
    if (expected && (!selected || std::string_view(expected) != selected)) return 3;
    const char* framing_value = std::getenv("CLAUDE_TEST_SELECTION_FRAMING");
    const std::string_view framing = framing_value ? framing_value : "";
    std::string pending_output;
    for (std::string line; std::getline(std::cin, line);) {
        const auto request = addon::parse_json(line, addon::Json_parse_limits{}, nullptr);
        if (!request || !request->find("request") || !request->find("request_id")) return 4;
        const auto* subtype = request->find("request")->find("subtype");
        if (!subtype || !subtype->is_string()) return 4;
        auto payload = addon::Json_value::object();
        if (subtype->as_string() == "initialize") {
            auto entry = addon::Json_value::object();
            entry.set("value", addon::Json_value("opus"));
            entry.set("resolvedModel", addon::Json_value("claude-native-opus"));
            auto models = addon::Json_value::array();
            models.push_back(std::move(entry));
            payload.set("models", std::move(models));
        }
        else
        if (subtype->as_string() == "get_settings") {
            if (std::string_view(mode) != "missing") {
                auto applied = addon::Json_value::object();
                applied.set("model", addon::Json_value(model));
                applied.set("effort", addon::Json_value(effort));
                payload.set("applied", std::move(applied));
            }
            payload.set("effective", addon::Json_value("private-settings-secret-canary"));
        }
        else return 5;   // The probe may neither prompt the model nor start Task MCP discovery.
        auto response = addon::Json_value::object();
        response.set("subtype", addon::Json_value(std::string_view(mode) == "unsupported" ? "error" : "success"));
        const bool initialize = subtype->as_string() == "initialize";
        const bool wrong_id = framing == (initialize ? "wrong_initialize_id" : "wrong_settings_id");
        response.set("request_id", wrong_id ? addon::Json_value("foreign-request") : *request->find("request_id"));
        response.set("response", std::move(payload));
        auto frame = addon::Json_value::object();
        frame.set("type", addon::Json_value("control_response"));
        frame.set("response", std::move(response));
        const auto reply = addon::serialize_json(frame) + '\n';
        std::string output = std::move(pending_output);
        pending_output.clear();
        if (!framing.empty()) {
            output += hook_frame(initialize ? "hook_started" : "hook_response");
        }
        if (initialize) {
            if (framing == "unknown_system") output += hook_frame("informational");
            if (framing == "empty_session")  output += hook_frame("hook_progress", "");
            if (framing == "foreign_session") output += hook_frame("hook_progress", "other-session");
            if (framing == "assistant_hook") {
                output += "{\"type\":\"assistant\",\"subtype\":\"hook_started\","
                    "\"session_id\":\"selection-session\"}\n";
            }
        }
        else
        if (framing == "foreign_settings_session") {
            output += hook_frame("hook_response", "other-session");
        }
        output += reply;
        if (framing == "hooks" || framing == "hooks_split") {
            const auto hook = hook_frame("hook_progress");
            if (initialize && framing == "hooks_split") {
                // Waiting for get_settings makes the hook straddle the probe's
                // control write without relying on pipe scheduling or sleeps.
                output += hook.substr(0, hook.size() / 2);
                pending_output = hook.substr(hook.size() / 2);
            }
            else {
                output += hook;
            }
        }
        if (initialize && framing == "premature_reply") output += reply;
        if (initialize && framing == "premature_reply_split") {
            output += reply.substr(0, reply.size() / 2);
            pending_output = reply.substr(reply.size() / 2);
        }
        std::cout << output << std::flush;
    }
    return 0;
}

bool write_record(std::string_view capture, const addon::Json_value& record)
{
    std::ofstream output(addon::path_from_utf8(capture), std::ios::binary | std::ios::app);
    output << addon::serialize_json(record) << '\n';
    return static_cast<bool>(output);
}

#ifdef _WIN32
std::string utf8(std::wstring_view text)
{
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
        nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
        result.data(), size, nullptr, nullptr);
    return result;
}

addon::Json_value arguments(int argc, wchar_t** argv)
{
    auto result = addon::Json_value::array();
    for (int index = 1; index < argc; ++index) {
        result.push_back(addon::Json_value(utf8(argv[index])));
    }
    return result;
}
#endif

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
{
    std::vector<std::string> probe_arguments;
    for (int index = 1; index < argc; ++index) probe_arguments.push_back(utf8(argv[index]));
    if (const auto result = selection_probe(probe_arguments)) return *result;
    std::vector<wchar_t> capture(32768);
    const DWORD length = GetEnvironmentVariableW(L"CLAUDE_TEST_ARGV_CAPTURE", capture.data(),
        static_cast<DWORD>(capture.size()));
    if (length == 0 || length >= capture.size()) {
        return 2;
    }
    auto record = addon::Json_value::object();
    record.set("argv", arguments(argc, argv));
    record.set("command_line", addon::Json_value(utf8(GetCommandLineW())));
    int parsed_count = 0;
    wchar_t** parsed = CommandLineToArgvW(GetCommandLineW(), &parsed_count);
    if (parsed == nullptr) {
        return 3;
    }
    record.set("command_line_argv", arguments(parsed_count, parsed));
    LocalFree(parsed);
    return write_record(utf8(std::wstring_view(capture.data(), length)), record) ? 0 : 3;
}
#else
int main(int argc, char** argv)
{
    if (const auto result = selection_probe(std::vector<std::string>(argv + 1, argv + argc))) return *result;
    const char* capture = std::getenv("CLAUDE_TEST_ARGV_CAPTURE");
    if (capture == nullptr || *capture == '\0') {
        return 2;
    }
    auto values = addon::Json_value::array();
    for (int index = 1; index < argc; ++index) {
        values.push_back(addon::Json_value(std::string(argv[index])));
    }
    auto record = addon::Json_value::object();
    record.set("argv", std::move(values));
    return write_record(capture, record) ? 0 : 3;
}
#endif
