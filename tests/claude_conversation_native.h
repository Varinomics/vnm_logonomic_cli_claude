#pragma once

#include "claude_accounts.h"
#include "windows_filesystem_test_support.h"

#include <logonomic_cli_addon/conversation_export.h>
#include <logonomic_cli_addon/package_source.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

namespace claude_native_test {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

inline bool check(bool condition, std::string_view detail)
{
    if (!condition) {
        std::cerr << "FAIL: " << detail << '\n';
    }
    return condition;
}

inline void write(const fs::path& path, std::string_view bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream << bytes;
    if (!stream) {
        throw std::runtime_error("native transcript fixture could not be written");
    }
}

inline std::string read(const fs::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("native conversation output could not be read");
    }
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

inline addon::Conversation_export_request request_for(const fs::path& profile)
{
    addon::Conversation_export_request request;
    request.provider_session_id = "native-Ω-漢字";
    request.launch_environment.platform_id = addon::host_platform_id();
    request.launch_environment.effective_environment = addon::Json_value::object();
    request.launch_environment.effective_environment.set(
        "CLAUDE_CONFIG_DIR", addon::Json_value(addon::path_to_utf8(profile)));
    return request;
}

inline std::string transcript(std::string_view id, std::string_view text = "Hello Ω")
{
    return R"({"type":"user","uuid":"prompt","parentUuid":null,"sessionId":)" +
        addon::serialize_json(addon::Json_value(std::string(id))) +
        R"(,"message":{"role":"user","content":)" +
        addon::serialize_json(addon::Json_value(std::string(text))) + "}}\n";
}

inline bool export_equivalence(const fs::path& root, const fs::path& output)
{
    const auto profile = root / addon::path_from_utf8("profile-Ω-漢字");
    auto request = request_for(profile);
    const auto source = profile / "projects/project-a" /
        addon::path_from_utf8(request.provider_session_id + ".jsonl");
    const auto id = addon::serialize_json(addon::Json_value(request.provider_session_id));
    write(source, transcript(request.provider_session_id) +
        R"({"type":"assistant","uuid":"progress","parentUuid":"prompt","sessionId":)" + id +
        R"(,"message":{"role":"assistant","stop_reason":"tool_use","content":"Working 漢字"}})" "\n" +
        R"({"type":"assistant","uuid":"final","parentUuid":"progress","sessionId":)" + id +
        R"(,"message":{"role":"assistant","stop_reason":"end_turn","content":"Done café"}})" "\n" +
        "{\"type\":");
    bool ok = true;
    std::string metadata;
    for (const bool compact : {false, true}) {
        for (const auto format : {addon::Conversation_export_format::MARKDOWN,
                 addon::Conversation_export_format::TEXT, addon::Conversation_export_format::CONVERSATION})
        {
            const auto directory = output / (compact ? "compact" : "full") /
                addon::conversation_export_format_name(format);
            fs::create_directories(directory);
            request.output_directory = addon::path_to_utf8(fs::canonical(directory));
            request.compact = compact;
            request.format = format;
            addon::Contract_failure failure;
            const auto result = claude::export_conversation(request, {}, &failure);
            ok &= check(result.has_value(), failure.reason_code + " " + failure.detail);
            if (!result) {
                continue;
            }
            const std::string expected = format == addon::Conversation_export_format::MARKDOWN
                ? "## User\n\nHello Ω\n\n" + std::string(compact ? "" : "## Assistant\n\nWorking 漢字\n\n") +
                    "## Assistant\n\nDone café\n\n"
                : format == addon::Conversation_export_format::TEXT
                    ? "User\n\nHello Ω\n\n" + std::string(compact ? "" : "Assistant\n\nWorking 漢字\n\n") +
                        "Assistant\n\nDone café\n\n"
                    : "#!vnm-chat 1\n\n::: user\nHello Ω\n:::\n\n" +
                        std::string(compact ? "" : "::: assistant commentary\nWorking 漢字\n:::\n\n") +
                        "::: assistant final\nDone café\n:::\n\n";
            const auto bytes = read(directory / result->file_name);
            ok &= check(bytes == expected, "complete native export matches the format's expected UTF-8 bytes");
            ok &= check(result->message_count == (compact ? 2 : 3) && result->attachment_count == 0 &&
                result->bytes == expected.size() && result->incomplete_tail_omitted,
                "complete native export reports exact counts, byte length and snapshot tail");
            ok &= check(!fs::exists(directory / (result->file_name + ".partial")),
                "successful close and final rename retain only completed output");
            metadata += std::string(compact ? "compact " : "full ") +
                addon::conversation_export_format_name(format) + " " + std::to_string(result->message_count) +
                " " + std::to_string(result->bytes) + "\n";
        }
    }
    write(output / "counts.txt", metadata);
    return ok;
}

inline bool filesystem(const fs::path& root)
{
    const auto profile = root / addon::path_from_utf8("native-profile-Ω-漢字");
    const auto project = profile / "projects/project-a";
    auto request = request_for(profile);
    const auto source = project / addon::path_from_utf8(request.provider_session_id + ".jsonl");
    write(source, transcript(request.provider_session_id));
    const auto output = root / "native-output";
    addon::Contract_failure failure;
    const auto extract = [
            &
        ](
            const std::function<bool()>& cancelled = {})
        {
            fs::remove_all(output);
            fs::create_directories(output);
            request.output_directory = addon::path_to_utf8(fs::canonical(output));
            return claude::export_conversation(request, cancelled, &failure);
        };
    bool ok = check(extract().has_value(), "Unicode profile, transcript filename and content export together");
    const auto unicode_alias = project / addon::path_from_utf8("NATIVE-ω-漢字.jsonl");
    fs::rename(source, unicode_alias);
    const bool native_alias = fs::is_regular_file(source);
    const auto aliased = extract();
    ok &= check(aliased.has_value() == native_alias &&
        (native_alias || failure.reason_code == "conversation_source_unavailable"),
        "Unicode filename case resolution follows the native filesystem");
    fs::rename(unicode_alias, source);
#ifdef _WIN32
    {
        filesystem_test::Access_denial deny_listing(project, FILE_LIST_DIRECTORY);
        std::error_code error;
        const fs::directory_iterator listing(project, error);
        ok &= check(error == std::errc::permission_denied,
            "Windows fixture actually denies project enumeration");
        std::ifstream known(source, std::ios::binary);
        ok &= check(known.good() && extract().has_value(),
            "known transcript access and export need no project enumeration permission");
    }
    {
        filesystem_test::Access_denial deny_read(source, FILE_READ_DATA);
        std::ifstream denied(source, std::ios::binary);
        ok &= check(!denied.good(), "Windows fixture actually denies the known transcript read");
        ok &= check(!extract() && failure.reason_code == "conversation_unavailable" && fs::is_empty(output),
            "a real transcript access error is reported without completed output");
    }
    ok &= check(!filesystem_test::restoration_failed, "temporary native access controls were restored");
#endif

    fs::remove(source);
    const auto linked = root / "linked-native.jsonl";
    write(linked, transcript(request.provider_session_id));
    std::error_code link_error;
    fs::create_symlink(linked, source, link_error);
    if (link_error == std::errc::permission_denied || link_error == std::errc::operation_not_supported ||
        link_error.value() == 1314)
    {
        std::cout << "SKIP: native symlink fixtures unavailable: " << link_error.message() << '\n';
    }
    else {
        ok &= check(!link_error, "native regular-file symlink fixture could be created");
        if (!link_error) {
            ok &= check(extract().has_value(), "a native regular-file symlink is a transcript candidate");
            fs::remove(linked);
            ok &= check(!extract() && failure.reason_code == "conversation_source_unavailable",
                "a dangling native symlink is a missing transcript");
            fs::remove(source);
            fs::create_symlink(source, source);
            bool status_failed = false;
            try {
                extract();
            }
            catch (const fs::filesystem_error&) {
                status_failed = true;
            }
            ok &= check(status_failed, "a real native candidate-status error is not hidden as missing history");
            fs::remove(source);
        }
    }
    write(source, transcript(request.provider_session_id));
    const auto account_profile = root / "account-profile";
    write(account_profile / "projects/project-a" / addon::path_from_utf8(request.provider_session_id + ".jsonl"),
        transcript(request.provider_session_id, "Selected account"));
    request.account_context = addon::Account_context{addon::k_account_context_version,
        claude::account_key(account_profile), addon::path_to_utf8(account_profile)};
    const auto selected = extract();
    ok &= check(selected && read(output / selected->file_name).find("Selected account") != std::string::npos,
        "bound account profile overrides the frozen launch environment");
    request.account_context->adapter_account_key = claude::account_key(profile);
    bool mismatch = false;
    try {
        extract();
    }
    catch (const std::runtime_error& error) {
        mismatch = std::string_view(error.what()) == "account_profile_mismatch";
    }
    ok &= check(mismatch, "account key and native profile cannot be mixed");
    request.account_context.reset();

    bool write_cancelled = false;
    const auto cancelled = extract([&] {
        write_cancelled = fs::exists(output / "conversation.md.partial");
        return write_cancelled;
    });
    ok &= check(!cancelled && write_cancelled && failure.reason_code == "cancelled" && fs::is_empty(output),
        "cancellation after output creation removes partial output");
    bool final_blocked = false;
    const auto rename_failed = extract([&] {
        if (!final_blocked && fs::exists(output / "conversation.md.partial")) {
            fs::create_directory(output / "conversation.md");
            final_blocked = true;
        }
        return false;
    });
    ok &= check(!rename_failed && final_blocked && failure.reason_code == "conversation_export_failed" &&
        failure.detail == "conversation output could not be finalized" &&
        !fs::exists(output / "conversation.md.partial"),
        "a genuine final-rename error fails export and removes its partial file");
    fs::remove_all(output);
    return ok;
}

} // namespace claude_native_test
