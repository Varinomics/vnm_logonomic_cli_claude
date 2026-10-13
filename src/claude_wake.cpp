#include "claude_wake.h"

#include "claude_support.h"

#include <logonomic_cli_addon/session_wake.h>
#include <logonomic_cli_addon/ids.h>

#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace claude {
namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
using Json = addon::Json_value;

constexpr auto k_bridge_timeout = std::chrono::seconds(5);

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
    bool supported_launch)
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
        const auto directory = parent / addon::random_nonce_hex();
        if (!fs::create_directory(directory)) {
            throw std::runtime_error("wake_directory_exists");
        }
#ifndef _WIN32
        fs::permissions(parent, fs::perms::owner_all, fs::perm_options::replace);
        fs::permissions(directory, fs::perms::owner_all, fs::perm_options::replace);
#endif
        const auto canonical = fs::canonical(directory);
        result.available = true;
        result.binding.set("directory", Json(path_string(canonical)));
        patch.set.emplace_back("VNM_CLAUDE_WAKE_DIRECTORY", path_string(canonical));
    }
    catch (const std::exception&) {
        result.reason = "claude_wake_storage_unavailable";
    }
    patch.session_wake = std::move(result);
}

void register_wake(const Json& event, const std::string& nonce)
{
    // Only the helper's host-accepted SessionStart publishes correlation.
    // No session capability or native inbox credential is persisted here.
    try {
#ifdef _WIN32
        const auto* value = _wgetenv(L"VNM_CLAUDE_WAKE_DIRECTORY");
#else
        const auto* value = std::getenv("VNM_CLAUDE_WAKE_DIRECTORY");
#endif
        if (!value || event.find("agent_id")) return;
        const fs::path directory(value);
        Json binding = Json::object();
        binding.set("directory", Json(path_string(directory)));
        if (!bridge_directory(binding)) return;
        Json registration = Json::object();
        registration.set("provider_session_id", Json(text(event, "session_id")));
        registration.set("transition_nonce", Json(nonce));
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
