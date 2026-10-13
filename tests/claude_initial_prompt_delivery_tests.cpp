// Delivery of an interactive-initial-prompt@1 text through the launcher shapes
// this package prepares. Each case prepares a launch through the actual producer,
// checks the patch as the host does, and runs the prepared invocation as the
// host's terminal backend starts it, with the Claude CLI replaced by
// claude_fake_cli, which records the arguments it receives. Claude must receive
// exactly the intended arguments: --add-dir with the brief's directory first and
// the prompt, byte for byte, last. The child PATH holds only this test's
// directories and the system shells, so no real Claude CLI can start.
#include "claude_preparation.h"
#include "claude_support.h"

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/json.h>
#include <logonomic_cli_addon/launch_preparation.h>
#include <logonomic_cli_addon/launcher_utilities.h>
#include <logonomic_cli_addon/package_source.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#endif

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef _WIN32
extern char** environ;
#endif

namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
using Json = addon::Json_value;

// Logonomic's launch-time title instruction, as the host sends it.
constexpr const char* k_title_instruction =
    "For this first substantive task, invoke the executable path in LOGONOMIC_VNMCTL_PATH once with arguments "
    "`set-title` and a concise 3-7 word title that omits the repository name. Only the root agent may set it, "
    "and a user title takes precedence. Do not inspect title state or retry; continue the task if it fails.";

class Context : public addon::Invocation_context
{
public:
    const addon::Invoke_request& request() const override { return m_request; }
    bool cancel_requested() const override { return false; }
    bool wait_for_cancel(int) override { return false; }
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
    bool m_finished = false;
};

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string utf8(const fs::path& path)
{
    return claude::path_string(path);
}

std::string listing(const std::vector<std::string>& values)
{
    auto array = Json::array();
    for (const auto& value : values) {
        array.push_back(Json(value));
    }
    return addon::serialize_json(array);
}

void write_file(const fs::path& path, std::string_view bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << bytes;
    require(static_cast<bool>(output), "cannot write " + utf8(path));
}

void copy_program(const fs::path& from, const fs::path& to)
{
    fs::create_directories(to.parent_path());
    fs::copy_file(from, to, fs::copy_options::overwrite_existing);
}

// The brief's and the package's directory names carry spaces, non-ASCII text
// (one character beyond the Basic Multilingual Plane) and each shell
// metacharacter the platform allows in a file name and the package admits.
struct Layout
{
    fs::path root;
    fs::path package_root;
    fs::path brief;
    fs::path tools;

    std::string text() const
    {
        return "Logonomic continuation: read the brief at " + utf8(brief) +
            " before anything else and follow it; do not modify the project until the user confirms.";
    }

    std::vector<std::string> expected_arguments(bool wake) const
    {
        std::vector<std::string> arguments{
            "--add-dir", utf8(brief.parent_path()), "--append-system-prompt", k_title_instruction,
            "--plugin-dir", utf8(package_root / "assets/claude-hooks")};
        if (wake) {
            arguments.insert(arguments.end(), {"--plugin-dir", utf8(package_root / "assets/claude-wake-dispatch")});
        }
        arguments.push_back(text());
        return arguments;
    }
};

Layout make_layout(const fs::path& root)
{
    const std::string punctuation =
        "Zo\xC3\xAB \xC3\x96l\xC3\xA7" "er \xE6\x97\xA5\xE6\x9C\xAC \xF0\x9F\x98\x80 O'Brien & Co (x) $HOME ^caret; "
        "#1 =x, ~y [z] {w} `tick"
#ifndef _WIN32
        " %PATH% !bang \"dq\" \\back *?|<>:"
#endif
        ;
    Layout layout;
    layout.root         = root;
    layout.package_root = root / addon::path_from_utf8("package " + punctuation);
    layout.brief        = root / addon::path_from_utf8("state " + punctuation) / "continuation_briefs" /
        "7f3c9a2e5b1d4f60" / "brief.md";
    layout.tools        = root / "tools";
    write_file(layout.brief, "# Continuation brief\n");
#ifdef _WIN32
    write_file(layout.tools / "node.exe", "");
#else
    write_file(layout.tools / "node", "");
#endif
    return layout;
}

std::vector<std::string> prepare(
    const Layout&                   layout,
    const std::string&              runner,
    const std::string&              script,
    const std::vector<std::string>& launcher_argv,
    bool                           wake)
{
    addon::Prepare_request request;
    request.launcher          = {script, runner, ""};
    request.working_directory = utf8(layout.root);
    request.title_instruction = k_title_instruction;
    addon::Launch_environment environment;
#ifdef _WIN32
    environment.platform_id = "windows-x86_64";
#else
    environment.platform_id = "linux-x86_64";
#endif
    environment.effective_environment = Json::object();
    environment.effective_environment.set("PATH", Json(utf8(layout.tools)));
    request.launch_environment = environment;
    request.launcher_argv      = launcher_argv;
    request.initial_prompt     = addon::Initial_prompt_request{layout.text(), utf8(layout.brief)};
    if (wake) {
        request.session_wake    = addon::Session_wake_offer{};
        request.profile_storage = addon::Profile_storage_context{1, utf8(layout.root / "profile")};
    }

    Context invocation;
    invocation.m_request.capability.capability_id = "launch-preparation";
    invocation.m_request.operation = "prepare";
    invocation.m_request.payload = addon::encode_prepare_request(request, 3);
    claude::handle_preparation(invocation, layout.package_root);
    require(invocation.m_finished && invocation.m_status == addon::Result_status::SUCCESS,
        runner + " launcher " + listing(launcher_argv) + " was refused: " + invocation.m_reason);
    addon::Contract_failure failure;
    const auto patch = addon::decode_launch_patch(invocation.m_payload, &failure, 3);
    require(patch && addon::validate_launch_patch(*patch, {}, {}, &failure) &&
        addon::validate_initial_prompt_acknowledgment(request.initial_prompt, *patch, &failure),
        "host rejects the prepared patch: " + failure.reason_code + ": " + failure.detail);
    require(!wake || (patch->session_wake && patch->session_wake->available),
        "the wake-enabled initial prompt retains its negotiated bridge");
    return *patch->argv;
}

std::vector<std::string> strings(const Json& array)
{
    std::vector<std::string> result;
    for (const auto& value : array.as_array()) {
        result.push_back(value.as_string());
    }
    return result;
}

Json read_capture(const fs::path& capture)
{
    std::ifstream input(capture, std::ios::binary);
    require(input.is_open(), "the fake Claude CLI never ran");
    const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    require(!bytes.empty() && bytes.find('\n') == bytes.size() - 1, "the fake Claude CLI must run exactly once");
    addon::Json_parse_error error;
    auto record = addon::parse_json(bytes, addon::Json_parse_limits{}, &error);
    require(record && record->is_object(), "unreadable capture record");
    return std::move(*record);
}

#ifdef _WIN32

// The host's terminal backend (vnm_terminal_surface windows_conpty_backend.cpp,
// quote_windows_argument and command_line_from_argv) gives the program native
// separators and quotes an argument only when it holds whitespace or a quote.
std::wstring host_command_line(const std::vector<std::string>& argv)
{
    std::wstring line;
    for (std::size_t index = 0; index < argv.size(); ++index) {
        std::wstring argument = addon::wide(argv[index]);
        if (index == 0) {
            std::replace(argument.begin(), argument.end(), L'/', L'\\');
        }
        if (!line.empty()) {
            line += L' ';
        }
        if (argument.empty()) {
            line += L"\"\"";
            continue;
        }
        if (argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
            line += argument;
            continue;
        }
        line += L'"';
        std::size_t backslashes = 0;
        for (const wchar_t character : argument) {
            if (character == L'\\') {
                ++backslashes;
                continue;
            }
            line.append(character == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
            backslashes = 0;
            line += character;
        }
        line.append(backslashes * 2, L'\\');
        line += L'"';
    }
    return line;
}

fs::path system_directory()
{
    std::vector<wchar_t> buffer(MAX_PATH + 1);
    const UINT length = GetSystemDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
    require(length > 0 && length < buffer.size(), "no system directory");
    return fs::path(std::wstring(buffer.data(), length));
}

std::optional<fs::path> search(const std::wstring& program)
{
    std::vector<wchar_t> buffer(32768);
    const DWORD length = SearchPathW(nullptr, program.c_str(), L".exe", static_cast<DWORD>(buffer.size()),
        buffer.data(), nullptr);
    if (length == 0 || length >= buffer.size()) {
        return std::nullopt;
    }
    return fs::path(std::wstring(buffer.data(), length));
}

// The host resolves a bare program through its search directories, trying the
// name as given and then with .exe.
fs::path host_executable(const std::string& program, const std::vector<fs::path>& directories)
{
    const auto candidate = addon::path_from_utf8(program);
    if (candidate.is_absolute()) {
        return candidate;
    }
    for (const auto& directory : directories) {
        for (const auto* suffix : {L"", L".exe"}) {
            auto path = directory / candidate;
            path += suffix;
            std::error_code error;
            if (fs::is_regular_file(path, error)) {
                return path;
            }
        }
    }
    throw std::runtime_error("cannot resolve " + program);
}

Json run(const std::vector<std::string>& argv, const fs::path& working_directory,
    const std::vector<fs::path>& path, const fs::path& capture)
{
    fs::remove(capture);
    std::vector<fs::path> directories{system_directory()};
    directories.insert(directories.end(), path.begin(), path.end());
    const auto application = host_executable(argv.front(), directories);
    std::wstring command_line = host_command_line(argv);

    std::vector<std::wstring> variables;
    wchar_t* block = GetEnvironmentStringsW();
    require(block != nullptr, "no environment block");
    for (const wchar_t* entry = block; *entry != L'\0'; entry += wcslen(entry) + 1) {
        const std::wstring variable(entry);
        const auto name = variable.substr(0, variable.find(L'=', 1));
        if (_wcsicmp(name.c_str(), L"PATH") == 0 || _wcsicmp(name.c_str(), L"CLAUDE_TEST_ARGV_CAPTURE") == 0 ||
            _wcsicmp(name.c_str(), L"PSExecutionPolicyPreference") == 0)
        {
            continue;
        }
        variables.push_back(variable);
    }
    FreeEnvironmentStringsW(block);
    std::wstring path_value;
    for (const auto& directory : directories) {
        if (!path_value.empty()) {
            path_value += L';';
        }
        path_value += directory.wstring();
    }
    variables.push_back(L"PATH=" + path_value);
    variables.push_back(L"CLAUDE_TEST_ARGV_CAPTURE=" + capture.wstring());
    // A Restricted execution policy would stop the npm .ps1 shim before it
    // runs; the policy gates scripts, not how their arguments are passed.
    variables.push_back(L"PSExecutionPolicyPreference=Bypass");
    std::sort(variables.begin(), variables.end(),
        [](const std::wstring& left, const std::wstring& right) { return _wcsicmp(left.c_str(), right.c_str()) < 0; });
    std::wstring environment;
    for (const auto& variable : variables) {
        environment += variable;
        environment += L'\0';
    }
    environment += L'\0';

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    require(CreateProcessW(application.c_str(), command_line.data(), nullptr, nullptr, FALSE,
        CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, environment.data(), working_directory.c_str(),
        &startup, &process) != FALSE, "cannot start " + utf8(application));
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, 60000);
    DWORD exit_code = 1;
    if (wait == WAIT_OBJECT_0) {
        GetExitCodeProcess(process.hProcess, &exit_code);
    }
    else {
        TerminateProcess(process.hProcess, 1);
    }
    CloseHandle(process.hProcess);
    require(wait == WAIT_OBJECT_0 && exit_code == 0,
        "the prepared invocation " + listing(argv) + " exited with " + std::to_string(exit_code));
    return read_capture(capture);
}

#else

// A system shell, found on this process's PATH. Claude is never looked up here:
// this PATH may reach a real CLI.
std::optional<fs::path> search(const std::string& program)
{
    const char* path = std::getenv("PATH");
    std::istringstream directories(path == nullptr ? "" : path);
    for (std::string directory; std::getline(directories, directory, ':');) {
        const auto candidate = fs::path(directory) / program;
        std::error_code error;
        if (!directory.empty() && fs::is_regular_file(candidate, error)) {
            return candidate;
        }
    }
    return std::nullopt;
}

// Shells arrive as absolute paths; a bare program resolves through the child's
// PATH, which names only this test's directories.
Json run(const std::vector<std::string>& argv, const fs::path&,
    const std::vector<fs::path>& path, const fs::path& capture)
{
    fs::remove(capture);
    auto program = fs::path(argv.front());
    if (!program.is_absolute()) {
        const auto found = std::find_if(path.begin(), path.end(), [&program](const fs::path& directory) {
            std::error_code error;
            return fs::is_regular_file(directory / program, error);
        });
        require(found != path.end(), "cannot resolve " + argv.front());
        program = *found / program;
    }
    std::vector<std::string> variables;
    for (char** entry = environ; *entry != nullptr; ++entry) {
        const std::string_view variable(*entry);
        if (!variable.starts_with("PATH=") && !variable.starts_with("CLAUDE_TEST_ARGV_CAPTURE=")) {
            variables.emplace_back(variable);
        }
    }
    std::string path_value;
    for (const auto& directory : path) {
        if (!path_value.empty()) {
            path_value += ':';
        }
        path_value += directory.string();
    }
    variables.push_back("PATH=" + path_value);
    variables.push_back("CLAUDE_TEST_ARGV_CAPTURE=" + capture.string());
    std::vector<char*> arguments;
    for (const auto& argument : argv) {
        arguments.push_back(const_cast<char*>(argument.c_str()));
    }
    arguments.push_back(nullptr);
    std::vector<char*> environment;
    for (auto& variable : variables) {
        environment.push_back(variable.data());
    }
    environment.push_back(nullptr);
    pid_t child = 0;
    require(posix_spawn(&child, program.c_str(), nullptr, nullptr, arguments.data(), environment.data()) == 0,
        "cannot start " + program.string());
    int status = 0;
    require(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "the prepared invocation " + listing(argv) + " did not exit cleanly");
    return read_capture(capture);
}

#endif

struct Delivery_case
{
    std::string              name;
    std::string              runner;
    std::string              script;          // the launcher's script body
    std::vector<std::string> launcher_argv;   // the host's resolution of runner and script
    std::vector<fs::path>    path;            // the child's PATH
    std::size_t              shim_arguments = 0;   // leading recorded arguments a shim adds
};

void deliver(const Layout& layout, const Delivery_case& delivery, bool wake)
{
    const auto argv = prepare(layout, delivery.runner, delivery.script, delivery.launcher_argv, wake);
    const auto capture = layout.root / "capture.jsonl";
    const auto record = run(argv, layout.root, delivery.path, capture);
    auto expected = layout.expected_arguments(wake);
    std::vector<std::string> parses{"argv"};
#ifdef _WIN32
    parses.push_back("command_line_argv");
#endif
    for (const auto& parse : parses) {
        const auto* recorded = record.find(parse);
        require(recorded != nullptr && recorded->is_array(), delivery.name + ": no " + parse + " recorded");
        auto received = strings(*recorded);
        require(received.size() >= delivery.shim_arguments, delivery.name + ": missing shim arguments");
        received.erase(received.begin(), received.begin() + static_cast<std::ptrdiff_t>(delivery.shim_arguments));
        require(received == expected, delivery.name + ": Claude's " + parse + " differs\n  prepared: " +
            listing(argv) + "\n  received: " + listing(received) + "\n  expected: " + listing(expected));
    }
    std::cout << "PASS " << delivery.name << (wake ? " with wake" : "") << '\n';
}

#ifdef _WIN32

// npm's cmd-shim output for Claude Code as it installs a native image, and its
// node-script form, which the CLI package used before, and its .ps1 shim.
constexpr std::string_view k_native_cmd_shim =
    "@ECHO off\r\nGOTO start\r\n:find_dp0\r\nSET dp0=%~dp0\r\nEXIT /b\r\n:start\r\nSETLOCAL\r\nCALL :find_dp0\r\n"
    "\"%dp0%\\node_modules\\@anthropic-ai\\claude-code\\bin\\claude.exe\"   %*\r\n";
constexpr std::string_view k_node_cmd_shim =
    "@ECHO off\r\nGOTO start\r\n:find_dp0\r\nSET dp0=%~dp0\r\nEXIT /b\r\n:start\r\nSETLOCAL\r\nCALL :find_dp0\r\n\r\n"
    "IF EXIST \"%dp0%\\node.exe\" (\r\n  SET \"_prog=%dp0%\\node.exe\"\r\n) ELSE (\r\n  SET \"_prog=node\"\r\n"
    "  SET PATHEXT=%PATHEXT:;.JS;=;%\r\n)\r\n\r\n"
    "endLocal & goto #_undefined_# 2>NUL || title %COMSPEC% & \"%_prog%\"  "
    "\"%dp0%\\node_modules\\@anthropic-ai\\claude-code\\cli.js\" %*\r\n";
constexpr std::string_view k_ps1_shim =
    "#!/usr/bin/env pwsh\n$basedir=Split-Path $MyInvocation.MyCommand.Definition -Parent\n\n$exe=\"\"\n"
    "if ($PSVersionTable.PSVersion -lt \"6.0\" -or $IsWindows) {\n"
    "  # Fix case when both the Windows and Linux builds of Node\n  # are installed in the same directory\n"
    "  $exe=\".exe\"\n}\n# Support pipeline input\nif ($MyInvocation.ExpectingInput) {\n"
    "  $input | & \"$basedir/node_modules/@anthropic-ai/claude-code/bin/claude.exe\"   $args\n} else {\n"
    "  & \"$basedir/node_modules/@anthropic-ai/claude-code/bin/claude.exe\"   $args\n}\nexit $LASTEXITCODE\n";

void run_cases(const Layout& layout, const fs::path& fake_cli)
{
    // npm cannot install below a directory whose name holds a cmd operator (its
    // shims break there), so the CLI directories carry spaces and non-ASCII only.
    const auto npm_native = layout.root / addon::path_from_utf8("npm native Zo\xC3\xAB \xE6\x97\xA5\xE6\x9C\xAC");
    const auto npm_node   = layout.root / addon::path_from_utf8("npm node Zo\xC3\xAB \xE6\x97\xA5\xE6\x9C\xAC");
    const auto npm_ps1    = layout.root / addon::path_from_utf8("npm ps1 Zo\xC3\xAB \xE6\x97\xA5\xE6\x9C\xAC");
    const auto native     = layout.root / addon::path_from_utf8("native Zo\xC3\xAB \xE6\x97\xA5\xE6\x9C\xAC");
    write_file(npm_native / "claude.cmd", k_native_cmd_shim);
    copy_program(fake_cli, npm_native / "node_modules/@anthropic-ai/claude-code/bin/claude.exe");
    write_file(npm_node / "claude.cmd", k_node_cmd_shim);
    copy_program(fake_cli, npm_node / "node.exe");
    write_file(npm_node / "node_modules/@anthropic-ai/claude-code/cli.js", "");
    write_file(npm_ps1 / "claude.ps1", k_ps1_shim);
    copy_program(fake_cli, npm_ps1 / "node_modules/@anthropic-ai/claude-code/bin/claude.exe");
    copy_program(fake_cli, native / "claude.exe");

    const auto windows_powershell = search(L"powershell.exe");
    require(windows_powershell.has_value(), "Windows PowerShell is not on PATH");
    const auto powershell_directory = windows_powershell->parent_path();
    const std::vector<std::string> powershell_argv{"powershell.exe", "-NoLogo", "-Command", "claude"};
    std::vector<Delivery_case> cases{
        {"cmd runner, npm shim of the native image", "cmd", "claude", {"cmd.exe", "/c", "claude"}, {npm_native}},
        {"cmd runner, npm node-script shim", "cmd", "claude", {"cmd.exe", "/c", "claude"}, {npm_node}, 1},
        {"cmd runner, native image", "cmd", "claude", {"cmd.exe", "/c", "claude"}, {native}},
        {"PowerShell runner, npm .ps1 shim", "powershell", "claude", powershell_argv, {npm_ps1, powershell_directory}},
        {"PowerShell runner, npm .cmd shim", "powershell", "claude", powershell_argv, {npm_native, powershell_directory}},
        {"PowerShell runner, native image", "powershell", "claude", powershell_argv, {native, powershell_directory}},
        {"direct launcher naming the native image", "direct", "\"" + utf8(native / "claude.exe") + "\"",
            {utf8(native / "claude.exe")}, {native}},
        {"direct launcher naming the npm .cmd shim", "direct", "claude.cmd", {"claude.cmd"}, {npm_native}},
    };
    if (const auto pwsh = search(L"pwsh.exe")) {
        cases.push_back({"pwsh launcher, npm .ps1 shim", "direct", "pwsh -NoProfile -Command claude",
            {"pwsh", "-NoProfile", "-Command", "claude"}, {npm_ps1, pwsh->parent_path()}});
    }
    else {
        std::cout << "SKIP pwsh launcher: pwsh.exe is not on PATH\n";
    }
    for (const auto& delivery : cases) {
        deliver(layout, delivery, false);
        deliver(layout, delivery, true);
    }
}

#else

void run_cases(const Layout& layout, const fs::path& fake_cli)
{
    const auto native = layout.root / addon::path_from_utf8("native Zo\xC3\xAB \xE6\x97\xA5\xE6\x9C\xAC");
    copy_program(fake_cli, native / "claude");
    fs::permissions(native / "claude", fs::perms::owner_exec, fs::perm_options::add);
    const auto bash = search("bash");
    const auto sh   = search("sh");
    require(bash && sh, "bash or sh is not on PATH");
    std::vector<Delivery_case> cases{
        {"direct launcher, PATH lookup", "direct", "claude", {"claude"}, {native}},
        {"bash runner", "bash", "claude", {bash->string(), "-c", "claude"}, {native}},
        {"sh launcher", "direct", "sh -c claude", {sh->string(), "-c", "claude"}, {native}},
    };
    for (const auto& delivery : cases) {
        deliver(layout, delivery, false);
        deliver(layout, delivery, true);
    }
}

#endif

} // namespace

int main(int argc, char** argv)
{
    const auto root = fs::temp_directory_path() / ("claude-prompt-delivery-" + addon::random_nonce_hex());
    try {
        require(argc == 2, "Expected the fake Claude CLI executable");
        fs::create_directories(root);
        const auto layout = make_layout(root);
        run_cases(layout, addon::path_from_utf8(argv[1]));
        std::cout << "Claude initial prompt delivery checks passed.\n";
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::error_code ignored;
        fs::remove_all(root, ignored);
        return 1;
    }
    std::error_code ignored;
    fs::remove_all(root, ignored);
    return 0;
}
