#include "claude_wake.h"

#include "claude_support.h"

#include <logonomic_cli_addon/session_wake.h>
#include <logonomic_cli_addon/ids.h>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace claude {
namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
using Json = addon::Json_value;

constexpr auto k_bridge_timeout = std::chrono::seconds(5);

struct Owner_status
{
    bool dead = false;
    std::string birth;
    std::uint64_t start = 0;
};

struct Wake_owner
{
    std::int64_t pid = 0;
    std::string birth;
};

Owner_status owner_status(std::int64_t pid)
{
    if (pid <= 0 || pid > std::numeric_limits<std::int32_t>::max()) return {};
#ifdef _WIN32
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
        FALSE, static_cast<DWORD>(pid));
    if (!process) return {GetLastError() == ERROR_INVALID_PARAMETER, {}};
    Owner_status result;
    const auto wait = WaitForSingleObject(process, 0);
    if (wait == WAIT_OBJECT_0) {
        result.dead = true;
    }
    else
    if (wait == WAIT_TIMEOUT) {
        FILETIME creation{}, exit{}, kernel{}, user{};
        if (GetProcessTimes(process, &creation, &exit, &kernel, &user)) {
            const auto stamp = (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32) |
                creation.dwLowDateTime;
            result.birth = std::to_string(stamp);
            result.start = stamp;
        }
    }
    CloseHandle(process);
    return result;
#else
    std::ifstream input("/proc/" + std::to_string(pid) + "/stat");
    std::string stat;
    if (!std::getline(input, stat)) {
        return {kill(static_cast<pid_t>(pid), 0) == -1 && errno == ESRCH, {}};
    }
    const auto end = stat.rfind(')');
    if (end == std::string::npos) return {};
    std::istringstream fields(stat.substr(end + 1));
    std::string value;
    for (int field = 3; field <= 22; ++field) {
        if (!(fields >> value)) return {};
        if (field == 3 && (value == "Z" || value == "X")) return {true, {}};
    }
    std::ifstream boot_input("/proc/sys/kernel/random/boot_id");
    std::string boot;
    if (!std::getline(boot_input, boot) || boot.empty()) return {};
    std::uint64_t start = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), start);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) return {};
    return {false, boot + "-" + value, start};
#endif
}

std::int64_t current_pid()
{
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return getpid();
#endif
}

std::int64_t parent_pid()
{
#ifdef _WIN32
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    std::int64_t parent = 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == GetCurrentProcessId()) {
                parent = entry.th32ParentProcessID;
                break;
            }
        }
        while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return parent;
#else
    // Reparenting to init cannot establish the original native owner.
    const auto parent = getppid();
    return parent > 1 ? parent : 0;
#endif
}

bool is_hex(std::string_view value, std::size_t size)
{
    return value.size() == size && std::all_of(value.begin(), value.end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
}

std::optional<fs::path> prospective_directory()
{
#ifdef _WIN32
    const auto* value = _wgetenv(L"VNM_CLAUDE_WAKE_DIRECTORY");
#else
    const auto* value = std::getenv("VNM_CLAUDE_WAKE_DIRECTORY");
#endif
    if (!value) return std::nullopt;
    const fs::path directory(value);
    std::error_code error;
    if (!directory.is_absolute() || !is_hex(path_string(directory.filename()), 32) ||
        fs::canonical(directory.parent_path(), error) != directory.parent_path() || error)
    {
        return std::nullopt;
    }
    return directory;
}

struct Wake_marker
{
    Wake_owner native;
    Wake_owner registrar;
};

bool same_owner(const Wake_owner& first, const Wake_owner& second)
{
    return first.pid == second.pid && first.birth == second.birth;
}

fs::path owner_marker(const fs::path& directory, const Wake_marker& marker)
{
    return directory.parent_path() / (path_string(directory.filename()) + "." +
        std::to_string(marker.native.pid) + "." + marker.native.birth + "." +
        std::to_string(marker.registrar.pid) + "." + marker.registrar.birth + ".owner");
}

std::optional<Wake_owner> parse_owner(std::string_view pid, std::string_view birth)
{
    Wake_owner owner;
    const auto parsed = std::from_chars(pid.data(), pid.data() + pid.size(), owner.pid);
    if (parsed.ec != std::errc{} || parsed.ptr != pid.data() + pid.size() || owner.pid <= 0 ||
        owner.pid > std::numeric_limits<std::int32_t>::max())
    {
        return std::nullopt;
    }
    if (birth.empty() || birth.size() > 64 || !std::all_of(birth.begin(), birth.end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || ch == '-';
    }))
    {
        return std::nullopt;
    }
    owner.birth = birth;
    return owner;
}

std::optional<Wake_marker> marker_owner(std::string_view name)
{
    std::vector<std::string_view> parts;
    while (true) {
        const auto dot = name.find('.');
        parts.push_back(name.substr(0, dot));
        if (dot == std::string_view::npos) break;
        name.remove_prefix(dot + 1);
    }
    if (parts.size() != 6 || !is_hex(parts[0], 32) || parts[5] != "owner") return std::nullopt;
    const auto native = parse_owner(parts[1], parts[2]);
    const auto registrar = parse_owner(parts[3], parts[4]);
    if (!native || !registrar) return std::nullopt;
    return Wake_marker{*native, *registrar};
}

std::optional<Wake_owner> anchored_owner(const fs::path& directory)
{
    const auto prefix = path_string(directory.filename()) + ".";
    std::optional<Wake_owner> owner;
    for (const auto& entry : fs::directory_iterator(directory.parent_path())) {
        const auto name = path_string(entry.path().filename());
        if (!name.starts_with(prefix)) continue;
        const auto marker = marker_owner(name);
        if (!marker || !fs::is_regular_file(entry.symlink_status()) ||
            (owner && !same_owner(*owner, marker->native)))
        {
            return std::nullopt;
        }
        owner = marker->native;
    }
    return owner;
}

bool publish_owner_marker(const fs::path& directory, const Wake_owner& native, const Wake_owner& registrar)
{
    const auto marker = owner_marker(directory, {native, registrar});
    const auto prefix = path_string(directory.filename()) + ".";
    const auto same_native = [&]() {
        for (const auto& entry : fs::directory_iterator(directory.parent_path())) {
            const auto name = path_string(entry.path().filename());
            if (!name.starts_with(prefix)) continue;
            const auto existing = marker_owner(name);
            if (!existing || !same_owner(existing->native, native) || !fs::is_regular_file(entry.symlink_status())) {
                return false;
            }
        }
        return true;
    };
    if (!same_native()) return false;
    // Complete identity is in the filename before any directory allocation.
    // The registrar keeps a still-publishing helper protected after native exit.
    std::ofstream output(marker, std::ios::binary);
    output.close();
    return output && same_native();
}

bool owner_dead(const Wake_owner& owner)
{
    const auto status = owner_status(owner.pid);
    return status.dead || (!status.birth.empty() && status.birth != owner.birth);
}

bool remove_bridge(const fs::path& directory)
{
    std::error_code error;
    const auto status = fs::symlink_status(directory, error);
    if (status.type() == fs::file_type::not_found) return true;
    if (error || !fs::is_directory(status) || fs::canonical(directory, error) != directory || error) return false;
    for (const auto& entry : fs::directory_iterator(directory)) {
        auto name = path_string(entry.path().filename());
        const auto temporary = name.rfind('.');
        if (temporary != std::string::npos && is_hex(std::string_view(name).substr(temporary + 1), 32)) {
            name.resize(temporary);
        }
        const auto dot = name.find('.');
        const auto stem = name.substr(0, dot);
        const auto suffix = dot == std::string::npos ? std::string{} : name.substr(dot);
        const bool message = is_hex(stem, 32) && (suffix == ".request" || suffix == ".claimed" ||
            suffix == ".response" || suffix == ".writing" || suffix == ".registration" ||
            suffix == ".submit" || suffix == ".submitted" || suffix == ".native-response");
        const bool receipt = is_hex(stem, 64) && suffix == ".receipt";
        if (name != "registration.json" && name != "dispatch-ready.json" && name != "active.json" &&
            !message && !receipt)
        {
            continue;
        }
        if (fs::is_regular_file(entry.symlink_status(error)) && !error) fs::remove(entry.path(), error);
    }
    return fs::remove(directory, error) && !error;
}

void sweep_dead_owners(const fs::path& parent)
{
    std::map<std::string, std::vector<std::pair<fs::path, std::optional<Wake_marker>>>> owners;
    for (const auto& entry : fs::directory_iterator(parent)) {
        if (!fs::is_regular_file(entry.symlink_status())) continue;
        const auto name = path_string(entry.path().filename());
        if (name.size() > 33 && name[32] == '.' && is_hex(name.substr(0, 32), 32) && name.ends_with(".owner")) {
            owners[name.substr(0, 32)].emplace_back(entry.path(), marker_owner(name));
        }
    }
    for (const auto& [nonce, markers] : owners) {
        const bool dead = std::all_of(markers.begin(), markers.end(), [](const auto& marker) {
            return marker.second && owner_dead(marker.second->native) && owner_dead(marker.second->registrar);
        });
        // Missing identity and access-denied process queries are not proof of
        // exit. In particular, prepared launches and retained terminals have
        // no expiry time.
        if (!dead || !remove_bridge(parent / nonce)) continue;
        std::error_code ignored;
        for (const auto& marker : markers) fs::remove(marker.first, ignored);
    }
}

std::optional<fs::path> bridge_directory(const Json& binding)
{
    if (!binding.is_object() || binding.as_object().size() != 1) {
        return std::nullopt;
    }
    const auto directory = addon::path_from_utf8(text(binding, "directory"));
    std::error_code error;
    if (!directory.is_absolute() || !fs::is_directory(fs::symlink_status(directory, error)) || error ||
        fs::canonical(directory, error) != directory || error)
    {
        return std::nullopt;
    }
    return directory;
}

std::optional<Json> read_response(const fs::path& path)
{
    std::error_code error;
    if (!fs::is_regular_file(fs::symlink_status(path, error)) || error ||
        fs::file_size(path, error) > 16384 || error)
    {
        return std::nullopt;
    }
    std::ifstream input(path, std::ios::binary);
    std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (input.bad()) {
        return std::nullopt;
    }
    return addon::parse_json(bytes, addon::Json_parse_limits{}, nullptr);
}

struct Exchange_result
{
    std::optional<Json> response;
    bool claimed = false;
};

Exchange_result exchange(
    addon::Invocation_context& context, const fs::path& directory, const Json& payload)
{
    const auto id       = addon::random_nonce_hex();
    const auto pending  = directory / (id + ".request");
    const auto writing  = directory / (id + ".writing");
    const auto claimed  = directory / (id + ".claimed");
    const auto response = directory / (id + ".response");
    Json request = Json::object();
    request.set("id", Json(id));
    request.set("operation", Json(context.request().operation));
    request.set("payload", payload);
    request.set("expires_at_ms", Json(now_ms() + 5000));
    {
        std::ofstream output(writing, std::ios::binary | std::ios::trunc);
        const auto bytes = addon::serialize_json(request);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.close();
        if (!output) {
            std::error_code ignored;
            fs::remove(writing, ignored);
            return {};
        }
    }
    std::error_code error;
    fs::rename(writing, pending, error);
    if (error) {
        fs::remove(writing, error);
        return {};
    }
    const auto deadline = std::chrono::steady_clock::now() + k_bridge_timeout;
    Exchange_result result;
    while (!context.cancel_requested() && std::chrono::steady_clock::now() < deadline) {
        if (auto value = read_response(response)) {
            result.response = std::move(value);
            break;
        }
        // The native Mod waits with fs.watch. This bounded adapter-side wait
        // avoids introducing a second platform-specific transport or daemon.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    // Removing an unclaimed request revokes it. Once claimed, an absent reply
    // is uncertain: the native scheduler may already have accepted the prompt.
    const bool revoked = fs::remove(pending, error);
    result.claimed = !revoked;
    fs::remove(response, error);
    if (result.response) {
        fs::remove(claimed, error);
    }
    return result;
}

} // namespace

void prepare_wake(
    addon::Launch_patch& patch,
    const std::optional<addon::Profile_storage_context>& storage,
    bool supported_launch, const fs::path& package_root)
{
    addon::Session_wake_preparation result;
    if (!supported_launch || !storage) {
        result.reason = "claude_wake_launch_unavailable";
        patch.session_wake = std::move(result);
        return;
    }
    try {
        const auto parent = addon::path_from_utf8(storage->root) / "wake";
        fs::create_directories(parent);
#ifndef _WIN32
        fs::permissions(parent, fs::perms::owner_all, fs::perm_options::replace);
#endif
        const auto canonical_parent = fs::canonical(parent);
        sweep_dead_owners(canonical_parent);
        const auto canonical = canonical_parent / addon::random_nonce_hex();
        result.available = true;
        result.binding.set("directory", Json(path_string(canonical)));
        patch.set.emplace_back("VNM_CLAUDE_WAKE_DIRECTORY", path_string(canonical));
#ifdef _WIN32
        const auto helper = package_root / "helpers/claude_helper.exe";
#else
        const auto helper = package_root / "helpers/claude_helper";
#endif
        patch.set.emplace_back("VNM_CLAUDE_WAKE_OWNER_HELPER", path_string(fs::absolute(helper)));
    }
    catch (const std::exception&) {
        result.reason = "claude_wake_storage_unavailable";
    }
    patch.session_wake = std::move(result);
}

bool anchor_wake_owner()
{
    try {
        const auto directory = prospective_directory();
        if (!directory) return false;
        const auto parent = parent_pid();
        const auto native = owner_status(parent);
        const auto registrar_pid = current_pid();
        const auto registrar = owner_status(registrar_pid);
        // This entry is invoked directly by the native Mod before classic
        // hooks, without a PATH-resolved Node launcher in the parent chain.
        if (native.birth.empty() || registrar.birth.empty() || native.start > registrar.start || parent_pid() != parent) {
            return false;
        }
        return publish_owner_marker(*directory, {parent, native.birth}, {registrar_pid, registrar.birth});
    }
    catch (const std::exception&) {
        return false;
    }
}

void register_wake(const Json& event, const std::string& nonce)
{
    // Only the helper's host-accepted SessionStart publishes correlation.
    // No session capability or native inbox credential is persisted here.
    try {
        if (event.find("agent_id")) return;
        const auto prospective = prospective_directory();
        if (!prospective) return;
        const auto& directory = *prospective;
        const auto owner = anchored_owner(directory);
        if (!owner || owner_status(owner->pid).birth != owner->birth) return;
        const auto registrar_pid = current_pid();
        const auto registrar_status = owner_status(registrar_pid);
        if (registrar_status.birth.empty()) return;
        if (!publish_owner_marker(directory, *owner, {registrar_pid, registrar_status.birth})) return;
        fs::create_directory(directory);
#ifndef _WIN32
        fs::permissions(directory, fs::perms::owner_all, fs::perm_options::replace);
#endif
        Json binding = Json::object();
        binding.set("directory", Json(path_string(directory)));
        if (!bridge_directory(binding)) return;
        Json registration = Json::object();
        registration.set("provider_session_id", Json(text(event, "session_id")));
        registration.set("transition_nonce", Json(nonce));
        registration.set("owner_pid", Json(owner->pid));
        registration.set("owner_birth", Json(owner->birth));
        const auto temporary = directory / (addon::random_nonce_hex() + ".registration");
        std::ofstream output(temporary, std::ios::binary);
        const auto bytes = addon::serialize_json(registration);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.close();
        if (!output) return;
        std::error_code ignored;
        fs::remove(directory / "registration.json", ignored);
        fs::rename(temporary, directory / "registration.json", ignored);
    }
    catch (const std::exception&) {
        // Failure disables wake alone; lifecycle publication already committed.
    }
}

void unregister_wake(const Json& event)
{
    try {
        if (event.find("agent_id")) return;
        const auto directory = prospective_directory();
        if (!directory) return;
        const auto owner = anchored_owner(*directory);
        if (!owner) return;
        const auto registration = read_response(*directory / "registration.json");
        if (registration) {
            const auto* pid = registration->find("owner_pid");
            if (text(*registration, "provider_session_id") != text(event, "session_id") ||
                text(*registration, "owner_birth") != owner->birth || !pid || !pid->is_integer() ||
                pid->as_integer() != owner->pid)
            {
                return;
            }
        }
        else {
            std::error_code error;
            if (fs::symlink_status(*directory, error).type() != fs::file_type::not_found) return;
        }
        if (remove_bridge(*directory)) {
            std::error_code ignored;
            const auto prefix = path_string(directory->filename()) + ".";
            for (const auto& entry : fs::directory_iterator(directory->parent_path())) {
                const auto name = path_string(entry.path().filename());
                const auto marker = name.starts_with(prefix) ? marker_owner(name) : std::nullopt;
                if (marker && same_owner(marker->native, *owner)) fs::remove(entry.path(), ignored);
            }
        }
    }
    catch (const std::exception&) {
        // A later prepare can reclaim a proven-dead owner after a failed close.
    }
}

void handle_wake(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    const bool inspect = context.request().operation == addon::k_session_wake_inspect_operation;
    if (!inspect && context.request().operation != addon::k_session_wake_notify_operation) {
        context.reject_unknown_operation();
        return;
    }
    std::optional<addon::Session_wake_target> target;
    if (inspect) {
        const auto input = addon::decode_session_wake_inspect_request(context.request().payload, &failure);
        if (input) {
            target = input->target;
        }
    }
    else {
        const auto input = addon::decode_session_wake_notify_request(context.request().payload, &failure);
        if (input) {
            target = input->target;
        }
    }
    if (!target) {
        context.fail(addon::Result_status::FAILED, failure.reason_code);
        return;
    }
    const auto directory = bridge_directory(target->binding);
    const auto registration = directory ? read_response(*directory / "registration.json") : std::nullopt;
    const bool matches = registration && text(*registration, "provider_session_id") == target->provider_session_id &&
        text(*registration, "transition_nonce") == target->correlation.transition_nonce;
    // Launcher arguments/environment are neither needed by the native bridge
    // nor appropriate to persist as transport messages.
    Json payload = Json::object();
    Json session = Json::object();
    session.set("provider_session_id", Json(target->provider_session_id));
    payload.set("session", std::move(session));
    payload.set("correlation", *context.request().payload.find("correlation"));
    if (!inspect) {
        payload.set("notification_id", *context.request().payload.find("notification_id"));
        payload.set("instruction", *context.request().payload.find("instruction"));
    }
    const auto outcome = matches ? exchange(context, *directory, payload) : Exchange_result{};
    if (inspect) {
        const auto readiness = outcome.response
            ? addon::decode_session_wake_readiness(*outcome.response, &failure) : std::nullopt;
        context.succeed(addon::encode_session_wake_readiness(readiness.value_or(
            addon::Session_wake_readiness{false, "claude_mod_not_ready"})));
    }
    else {
        const auto notified = outcome.response
            ? addon::decode_session_wake_notify_result(*outcome.response, &failure) : std::nullopt;
        context.succeed(addon::encode_session_wake_notify_result(notified.value_or(
            addon::Session_wake_notify_result{
                outcome.claimed ? addon::Session_wake_disposition::UNCERTAIN :
                    addon::Session_wake_disposition::UNAVAILABLE,
                outcome.claimed ? "claude_wake_result_unknown" : "claude_mod_not_ready", std::nullopt})));
    }
}

} // namespace claude
