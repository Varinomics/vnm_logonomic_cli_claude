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

#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace addon = logonomic::cli_addon;

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
