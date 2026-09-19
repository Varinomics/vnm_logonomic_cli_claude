#include <logonomic_cli_addon/json.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>

std::string setting(const char* name, const char* fallback)
{
    const char* value = std::getenv(name);
    return value ? value : fallback;
}

int main(int argc, char** argv)
{
    namespace addon = logonomic::cli_addon;
    const auto capture = setting("CLAUDE_TEST_CAPTURE", "");
    if (argc < 2 || capture.empty()) return 2;
    const std::string operation = argv[1];
    if (operation == "provider-event" && argc == 4 && std::string(argv[2]) == "acknowledge") {
        std::ofstream(capture, std::ios::app) << "{\"operation\":\"acknowledge\"}\n";
        return 0;
    }
    const std::string bytes{std::istreambuf_iterator<char>(std::cin), {}};
    const auto request = addon::parse_json(bytes, {}, nullptr);
    if (!request || !request->is_object()) return 2;
    auto record = addon::Json_value::object();
    record.set("operation", addon::Json_value(operation));
    record.set("request", *request);
    std::ofstream(capture, std::ios::app) << addon::serialize_json(record) << '\n';
    if (operation == "request-title-instruction") {
        const auto mode = setting("CLAUDE_TEST_CONTEXT", "non-title");
        if (mode == "stall") {
            std::this_thread::sleep_for(std::chrono::seconds(60));
            return 4;
        }
        if (mode == "refused") {
            std::cout << "{\"accepted\":false,\"reason\":\"session_unavailable\"}\n";
            return 0;
        }
        auto response = addon::Json_value::object();
        response.set("accepted", addon::Json_value(true));
        response.set("instruction_required", addon::Json_value(mode == "title"));
        response.set("instruction", addon::Json_value(mode == "empty" ? "" : "Host changed-files and delegation.\nExact notice text."));
        std::cout << addon::serialize_json(response) << '\n';
        return 0;
    }
    if (operation != "provider-event" || argc != 4 || std::string(argv[2]) != "deliver") return 2;
    const auto mode = setting("CLAUDE_TEST_LIFECYCLE", "accepted");
    if (mode == "transport") return 4;
    if (mode == "stall") {
        std::this_thread::sleep_for(std::chrono::seconds(60));
        return 4;
    }
    if (mode == "refused") std::cout << "{\"accepted\":false,\"reason\":\"unsupported_root_correlation\",\"epoch\":1}\n";
    else std::cout << "{\"accepted\":true,\"epoch\":1}\n";
    return 0;
}
