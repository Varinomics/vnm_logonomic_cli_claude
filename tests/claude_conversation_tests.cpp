#include "claude_conversation.h"

#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/package_source.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

bool check(bool condition, std::string_view detail)
{
    if (!condition) {
        std::cerr << "FAIL: " << detail << '\n';
    }
    return condition;
}

void write(const fs::path& path, std::string_view bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << bytes;
}

std::string read(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::size_t occurrences(std::string_view haystack, std::string_view needle)
{
    std::size_t count = 0;
    for (auto offset = haystack.find(needle); offset != std::string_view::npos;
         offset = haystack.find(needle, offset + needle.size()))
    {
        ++count;
    }
    return count;
}

bool compact_projection(const fs::path& root, const fs::path& source,
    addon::Conversation_export_request request)
{
    const std::string body =
        R"({"type":"user","uuid":"prompt","parentUuid":null,"sessionId":"session-a","message":{"role":"user","content":"Visible prompt"}})" "\n"
        R"({"type":"assistant","uuid":"call","parentUuid":"prompt","sessionId":"session-a","message":{"id":"call-response","role":"assistant","stop_reason":"tool_use","content":[{"type":"text","text":"Intermediate progress"},{"type":"thinking","thinking":"","signature":"opaque-empty-verification"},{"type":"thinking","thinking":"Reasoning detail"},{"type":"tool_use","id":"search-a","name":"ToolSearch","input":{"query":"Read"}}]}})" "\n"
        R"({"type":"user","uuid":"result","parentUuid":"call","sessionId":"session-a","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"search-a","content":[{"type":"text","text":"Tool result text"},{"type":"tool_reference","tool_name":"Read"}]}]}})" "\n"
        R"({"type":"assistant","uuid":"encrypted","parentUuid":"result","sessionId":"session-a","message":{"id":"redacted-response","role":"assistant","content":[{"type":"redacted_thinking","data":"ciphertext-one"}]}})" "\n"
        R"({"type":"assistant","uuid":"empty-thought","parentUuid":"encrypted","sessionId":"session-a","message":{"id":"empty-response","role":"assistant","content":[{"type":"thinking","thinking":"","signature":"opaque-verification"}]}})" "\n"
        R"({"type":"assistant","uuid":"final","parentUuid":"empty-thought","sessionId":"session-a","message":{"id":"final-response","role":"assistant","stop_reason":"end_turn","content":[{"type":"text","text":"Visible final reply"}]}})" "\n";
    write(source, body);
    bool ok = true;
    for (const auto format : {addon::Conversation_export_format::MARKDOWN, addon::Conversation_export_format::TEXT,
             addon::Conversation_export_format::CONVERSATION})
    {
        for (const bool compact : {false, true}) {
            request.format = format;
            request.compact = compact;
            request.output_directory = addon::path_to_utf8(root / "projection" / (compact ? "compact" : "full") /
                addon::conversation_export_format_name(format));
            fs::create_directories(addon::path_from_utf8(request.output_directory));
            addon::Contract_failure failure;
            const auto result = claude::export_conversation(request, {}, &failure);
            ok &= check(result.has_value(), failure.reason_code + " " + failure.detail);
            if (!result) {
                continue;
            }
            const auto text = read(addon::path_from_utf8(request.output_directory) / result->file_name);
            ok &= check(result->compact == compact && result->message_count == (compact ? 3 : 5),
                "compact counts user, final, and encrypted messages without tool activity");
            for (const auto* retained : {"Visible prompt", "Visible final reply", "[Encrypted message]"}) {
                ok &= check(occurrences(text, retained) == 1, std::string(retained) + " retained once");
            }
            for (const auto* hidden : {"Intermediate progress", "Reasoning detail", "Tool result text", "tool_reference"}) {
                ok &= check((text.find(hidden) == std::string::npos) == compact,
                    "full export includes nested tool references while compact excludes internal content");
            }
            ok &= check(text.find("ciphertext-one") == std::string::npos,
                "redacted content exports its placeholder rather than encrypted bytes");
            ok &= check(occurrences(text, "Thinking") == (compact ? 0 : 1),
                "empty thinking adds no heading or empty message beside retained reasoning");
            if (format == addon::Conversation_export_format::CONVERSATION) {
                ok &= check(text.find("::: assistant final\nVisible final reply") != std::string::npos,
                    "the native final phase reaches the conversation format");
            }
        }
    }
    request.output_directory = addon::path_to_utf8(root / "invalid-tool-reference");
    fs::create_directories(addon::path_from_utf8(request.output_directory));
    write(source,
        R"({"type":"user","uuid":"result","parentUuid":null,"sessionId":"session-a","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"search-a","content":[{"type":"tool_reference"}]}]}})" "\n");
    addon::Contract_failure failure;
    ok &= check(!claude::export_conversation(request, {}, &failure) &&
        failure.reason_code == "conversation_content_invalid",
        "malformed nested tool content fails even when compact would omit that block");
    ok &= check(fs::is_empty(addon::path_from_utf8(request.output_directory)),
        "a parser failure publishes neither partial nor final export");
    return ok;
}

bool advisor_results(const fs::path& root, const fs::path& source,
    addon::Conversation_export_request request)
{
    // Claude Code's advisor emits native assistant blocks for the server call
    // and the plaintext, encrypted, or error variants defined by Anthropic.
    write(source,
        R"({"type":"user","uuid":"prompt","parentUuid":null,"sessionId":"session-a","message":{"role":"user","content":"Review the worker pool"}})" "\n"
        R"({"type":"assistant","uuid":"call","parentUuid":"prompt","sessionId":"session-a","message":{"id":"advised-response","role":"assistant","content":[{"type":"server_tool_use","id":"srvtoolu_advice","name":"advisor","input":{}}]}})" "\n"
        R"({"type":"assistant","uuid":"advice","parentUuid":"call","sessionId":"session-a","message":{"id":"advised-response","role":"assistant","content":[{"type":"advisor_tool_result","tool_use_id":"srvtoolu_advice","content":{"type":"advisor_result","text":"Drain the queue before joining workers","stop_reason":"end_turn"}}]}})" "\n"
        R"({"type":"assistant","uuid":"encrypted-call","parentUuid":"advice","sessionId":"session-a","message":{"id":"advised-response","role":"assistant","content":[{"type":"server_tool_use","id":"srvtoolu_encrypted","name":"advisor","input":{}}]}})" "\n"
        R"({"type":"assistant","uuid":"encrypted","parentUuid":"encrypted-call","sessionId":"session-a","message":{"id":"advised-response","role":"assistant","content":[{"type":"advisor_tool_result","tool_use_id":"srvtoolu_encrypted","content":{"type":"advisor_redacted_result","encrypted_content":"private-advisor-ciphertext"}}]}})" "\n"
        R"({"type":"assistant","uuid":"error-call","parentUuid":"encrypted","sessionId":"session-a","message":{"id":"advised-response","role":"assistant","content":[{"type":"server_tool_use","id":"srvtoolu_error","name":"advisor","input":{}}]}})" "\n"
        R"({"type":"assistant","uuid":"error","parentUuid":"error-call","sessionId":"session-a","message":{"id":"advised-response","role":"assistant","content":[{"type":"advisor_tool_result","tool_use_id":"srvtoolu_error","content":{"type":"advisor_tool_result_error","error_code":"overloaded"}}]}})" "\n"
        R"({"type":"assistant","uuid":"final","parentUuid":"error","sessionId":"session-a","message":{"id":"advised-response","role":"assistant","stop_reason":"end_turn","content":[{"type":"text","text":"Worker pool review complete"}]}})" "\n");
    bool ok = true;
    for (const auto format : {addon::Conversation_export_format::MARKDOWN, addon::Conversation_export_format::TEXT,
             addon::Conversation_export_format::CONVERSATION})
    {
        for (const bool compact : {false, true}) {
            request.format = format;
            request.compact = compact;
            request.output_directory = addon::path_to_utf8(root / "advisor" / (compact ? "compact" : "full") /
                addon::conversation_export_format_name(format));
            fs::create_directories(addon::path_from_utf8(request.output_directory));
            addon::Contract_failure failure;
            const auto result = claude::export_conversation(request, {}, &failure);
            ok &= check(result.has_value(), "advisor result export: " + failure.reason_code + " " + failure.detail);
            if (!result) {
                continue;
            }
            const auto text = read(addon::path_from_utf8(request.output_directory) / result->file_name);
            ok &= check(result->message_count == (compact ? 3 : 8), "advisor blocks retain native message boundaries");
            ok &= check(occurrences(text, "[Encrypted message]") == 1 &&
                text.find("private-advisor-ciphertext") == std::string::npos,
                "encrypted advisor guidance retains its placeholder in both projections");
            for (const auto* hidden : {"Drain the queue before joining workers", "overloaded"}) {
                ok &= check((text.find(hidden) == std::string::npos) == compact,
                    "full export retains advisor guidance and errors while compact omits tool content");
            }
            ok &= check(text.find("Review the worker pool") != std::string::npos &&
                text.find("Worker pool review complete") != std::string::npos,
                "server tool results do not prevent exporting the user prompt and final reply");
        }
    }
    request.output_directory = addon::path_to_utf8(root / "invalid-advisor");
    fs::create_directories(addon::path_from_utf8(request.output_directory));
    write(source,
        R"({"type":"assistant","uuid":"advice","parentUuid":null,"sessionId":"session-a","message":{"role":"assistant","content":[{"type":"advisor_tool_result","tool_use_id":"srvtoolu_advice","content":{"type":"advisor_result"}}]}})" "\n");
    addon::Contract_failure failure;
    ok &= check(!claude::export_conversation(request, {}, &failure) &&
        failure.reason_code == "conversation_content_invalid", "incomplete advisor guidance fails explicitly");
    ok &= check(fs::is_empty(addon::path_from_utf8(request.output_directory)),
        "malformed advisor content leaves no completed or partial output");
    return ok;
}

bool empty_projection(const fs::path& root, const fs::path& source,
    addon::Conversation_export_request request)
{
    // Anthropic ToolResultBlockParam permits omitted content and an empty
    // iterable. ThinkingBlock.signature is opaque verification, not ciphertext.
    bool ok = true;
    for (const bool omitted : {true, false}) {
        write(source,
            R"({"type":"user","uuid":"prompt","parentUuid":null,"sessionId":"session-a","message":{"role":"user","content":"Inspect the empty file"}})" "\n"
            R"({"type":"assistant","uuid":"call","parentUuid":"prompt","sessionId":"session-a","message":{"role":"assistant","stop_reason":"tool_use","content":[{"type":"tool_use","id":"read-empty","name":"Read","input":{"file_path":"empty.txt"}}]}})" "\n" +
            std::string(omitted
                ? R"({"type":"user","uuid":"result","parentUuid":"call","sessionId":"session-a","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"read-empty"}]}})" "\n"
                : R"({"type":"user","uuid":"result","parentUuid":"call","sessionId":"session-a","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"read-empty","content":[]}]}})" "\n") +
            R"({"type":"assistant","uuid":"empty-thought","parentUuid":"result","sessionId":"session-a","message":{"role":"assistant","content":[{"type":"thinking","thinking":"","signature":"opaque-verification"}]}})" "\n"
            R"({"type":"assistant","uuid":"thought","parentUuid":"empty-thought","sessionId":"session-a","message":{"role":"assistant","content":[{"type":"thinking","thinking":"The file has no bytes","signature":"other-verification"}]}})" "\n"
            R"({"type":"assistant","uuid":"answer","parentUuid":"thought","sessionId":"session-a","message":{"role":"assistant","stop_reason":"end_turn","content":[{"type":"thinking","thinking":"","signature":"final-verification"},{"type":"text","text":"The file is empty"}]}})" "\n");
        for (const auto format : {addon::Conversation_export_format::MARKDOWN, addon::Conversation_export_format::TEXT,
                 addon::Conversation_export_format::CONVERSATION})
        {
            for (const bool compact : {false, true}) {
                request.format = format;
                request.compact = compact;
                request.output_directory = addon::path_to_utf8(root / (omitted ? "empty-omitted" : "empty-array") /
                    (compact ? "compact" : "full") / addon::conversation_export_format_name(format));
                fs::create_directories(addon::path_from_utf8(request.output_directory));
                addon::Contract_failure failure;
                const auto result = claude::export_conversation(request, {}, &failure);
                ok &= check(result.has_value(), "empty native content exports: " + failure.reason_code + " " + failure.detail);
                if (!result) {
                    continue;
                }
                const auto text = read(addon::path_from_utf8(request.output_directory) / result->file_name);
                ok &= check(result->message_count == (compact ? 2 : 4),
                    "empty projections do not add messages or abort the conversation");
                ok &= check(occurrences(text, "Thinking") == (compact ? 0 : 1) &&
                    (text.find("The file has no bytes") == std::string::npos) == compact,
                    "only nonempty reasoning contributes a Thinking block");
                ok &= check(text.find("[Encrypted message]") == std::string::npos &&
                    text.find("verification") == std::string::npos,
                    "an ordinary thinking signature does not imply encrypted content");
                ok &= check(occurrences(text, "Inspect the empty file") == 1 &&
                    occurrences(text, "The file is empty") == 1,
                    "discussion before and after empty provider rows is retained once");
            }
        }
    }
    return ok;
}

bool malformed_empty_results(const fs::path& root, const fs::path& source,
    addon::Conversation_export_request request)
{
    // ToolResultBlockParam requires a string tool_use_id and permits absent,
    // string or iterable content. An empty projection cannot bypass that schema.
    bool ok = true;
    std::size_t variant = 0;
    for (const auto* result : {
             R"({"type":"tool_result"})",
             R"({"type":"tool_result","tool_use_id":17,"content":[]})",
             R"({"type":"tool_result","tool_use_id":"read-empty","content":null})"})
    {
        const auto directory = "invalid-empty-result-" + std::to_string(variant++);
        write(source,
            R"({"type":"user","uuid":"prompt","parentUuid":null,"sessionId":"session-a","message":{"role":"user","content":"Read the file"}})" "\n" +
            std::string(R"({"type":"user","uuid":"result","parentUuid":"prompt","sessionId":"session-a","message":{"role":"user","content":[)") +
            result + "]}}\n" +
            R"({"type":"assistant","uuid":"answer","parentUuid":"result","sessionId":"session-a","message":{"role":"assistant","content":"The file is empty"}})" "\n");
        for (const auto format : {addon::Conversation_export_format::MARKDOWN, addon::Conversation_export_format::TEXT,
                 addon::Conversation_export_format::CONVERSATION})
        {
            for (const bool compact : {false, true}) {
                request.format = format;
                request.compact = compact;
                request.output_directory = addon::path_to_utf8(root / directory /
                    (compact ? "compact" : "full") / addon::conversation_export_format_name(format));
                fs::create_directories(addon::path_from_utf8(request.output_directory));
                addon::Contract_failure failure;
                ok &= check(!claude::export_conversation(request, {}, &failure) &&
                    failure.reason_code == "conversation_content_invalid",
                    "empty tool results retain native identity and content validation");
                ok &= check(fs::is_empty(addon::path_from_utf8(request.output_directory)),
                    "a malformed empty tool result publishes no partial or completed conversation");
            }
        }
    }
    return ok;
}

} // namespace

int main()
{
    // Oracle: Anthropic Agent SDK transcript parent chains and parallel tool
    // recovery. A response has separate UUIDs for blocks sharing message.id.
    // Primary source paths and compaction semantics: docs/conversation_export.md.
    const auto root = fs::temp_directory_path() / ("claude-conversation-" + addon::random_nonce_hex());
    const auto profile = root / "profile";
    const auto source = profile / "projects/project-a/session-a.jsonl";
    const std::string body =
        R"({"type":"user","uuid":"prompt","parentUuid":null,"sessionId":"session-a","forkedFrom":{"sessionId":"parent","messageUuid":"original"},"message":{"role":"user","content":[{"type":"text","text":"Inherited user prompt Ω"},{"type":"image","source":{"type":"url","url":"https://example.invalid/image.png"}}]}})" "\n"
        R"({"type":"assistant","uuid":"old","parentUuid":"prompt","sessionId":"session-a","message":{"id":"abandoned-response","role":"assistant","content":[{"type":"text","text":"Abandoned branch"}]}})" "\n"
        R"({"type":"assistant","uuid":"call-a","parentUuid":"prompt","sessionId":"session-a","timestamp":"2026-10-23T12:00:00Z","message":{"id":"parallel-response","role":"assistant","content":[{"type":"tool_use","id":"read-a","name":"Read","input":{"file_path":"first.txt"}}]}})" "\n"
        R"({"type":"assistant","uuid":"call-b","parentUuid":"call-a","sessionId":"session-a","timestamp":"2026-10-23T12:00:01Z","message":{"id":"parallel-response","role":"assistant","content":[{"type":"tool_use","id":"read-b","name":"Read","input":{"file_path":"second.txt"}}]}})" "\n"
        R"({"type":"user","uuid":"result-b","parentUuid":"call-b","sessionId":"session-a","timestamp":"2026-10-23T12:00:02Z","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"read-b","content":[{"type":"text","text":"Second result\nwith newline"}]}]}})" "\n"
        R"({"type":"user","uuid":"result-a","parentUuid":"call-a","sessionId":"session-a","timestamp":"2026-10-23T12:00:03Z","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"read-a","content":"First result"}]}})" "\n"
        R"({"type":"assistant","uuid":"answer","parentUuid":"result-a","sessionId":"session-a","message":{"id":"final-response","role":"assistant","content":[{"type":"text","text":"Final answer\n```md\n# preserved\n```"}]}})" "\n"
        R"({"type":"assistant","uuid":"side","parentUuid":null,"sessionId":"session-a","isSidechain":true,"agentId":"helper","message":{"id":"side-response","role":"assistant","content":[{"type":"text","text":"Sidechain text"}]}})" "\n";
    write(source, body + "{\"type\":");
    addon::Conversation_export_request request;
    request.provider_session_id = "session-a";
    request.launch_environment.platform_id = addon::host_platform_id();
    request.launch_environment.effective_environment = addon::Json_value::object();
    request.launch_environment.effective_environment.set("CLAUDE_CONFIG_DIR", addon::Json_value(addon::path_to_utf8(profile)));
    bool ok = true;
    for (const auto format : {addon::Conversation_export_format::MARKDOWN, addon::Conversation_export_format::TEXT,
             addon::Conversation_export_format::CONVERSATION})
    {
        request.format = format;
        request.output_directory = addon::path_to_utf8(root / addon::conversation_export_format_name(format));
        fs::create_directories(addon::path_from_utf8(request.output_directory));
        addon::Contract_failure failure;
        const auto result = claude::export_conversation(request, {}, &failure);
        ok &= check(result.has_value(), failure.reason_code + " " + failure.detail);
        if (result) {
            const auto text = read(addon::path_from_utf8(request.output_directory) / result->file_name);
            ok &= check(result->message_count == 6 && result->attachment_count == 1, "parallel message and attachment counts");
            ok &= check(result->incomplete_tail_omitted, "in-progress partial tail reported");
            for (const auto* expected : {"Inherited user prompt Ω", "first.txt", "second.txt", "First result", "Second result"}) {
                ok &= check(occurrences(text, expected) == 1, std::string(expected) + " retained exactly once");
            }
            ok &= check(text.find("Abandoned branch") == std::string::npos &&
                text.find("Sidechain text") == std::string::npos, "selected main branch excludes other conversations");
            ok &= check(text.find("Second result") < text.find("Final answer"), "parallel tool result precedes continuation");
        }
    }
    // Native _recover_orphaned_parallel_tool_results collects every direct
    // result before considering strays, regardless of their earlier file order.
    const std::string parallel_prefix =
        R"({"type":"user","uuid":"prompt","parentUuid":null,"sessionId":"session-a","message":{"role":"user","content":"Prompt"}})" "\n"
        R"({"type":"assistant","uuid":"call-a","parentUuid":"prompt","sessionId":"session-a","message":{"id":"parallel","role":"assistant","content":[{"type":"tool_use","id":"a","name":"Read","input":{"path":"first.txt"}}]}})" "\n"
        R"({"type":"assistant","uuid":"call-b","parentUuid":"call-a","sessionId":"session-a","message":{"id":"parallel","role":"assistant","content":[{"type":"tool_use","id":"b","name":"Read","input":{"path":"second.txt"}}]}})" "\n"
        R"({"type":"user","uuid":"stray-a","parentUuid":"not-in-this-response","sessionId":"session-a","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"a","content":"Off-branch result A"}]}})" "\n";
    const std::string direct_a =
        R"({"type":"user","uuid":"direct-a","parentUuid":"call-a","sessionId":"session-a","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"a","content":"Canonical result A"}]}})" "\n";
    const std::string parallel_suffix =
        R"({"type":"user","uuid":"direct-b","parentUuid":"call-b","sessionId":"session-a","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"b","content":"Canonical result B"}]}})" "\n"
        R"({"type":"assistant","uuid":"answer","parentUuid":"direct-b","sessionId":"session-a","message":{"id":"answer-response","role":"assistant","content":"Parallel continuation"}})" "\n";
    for (const bool has_direct_result : {true, false}) {
        write(source, parallel_prefix + (has_direct_result ? direct_a : "") + parallel_suffix);
        for (const auto format : {addon::Conversation_export_format::MARKDOWN,
                 addon::Conversation_export_format::TEXT})
        {
            request.format = format;
            request.output_directory = addon::path_to_utf8(root /
                (has_direct_result ? "parallel-direct" : "parallel-stray") /
                addon::conversation_export_format_name(format));
            fs::create_directories(addon::path_from_utf8(request.output_directory));
            addon::Contract_failure error;
            const auto result = claude::export_conversation(request, {}, &error);
            ok &= check(result && result->message_count == 6,
                "parallel replay retains exactly one result for each call");
            if (result) {
                const auto text = read(addon::path_from_utf8(request.output_directory) / result->file_name);
                const auto* expected = has_direct_result ? "Canonical result A" : "Off-branch result A";
                ok &= check(occurrences(text, expected) == 1 && occurrences(text, "Canonical result B") == 1,
                    "direct results win, with stray recovery only for an unanswered call");
                ok &= check(!has_direct_result || text.find("Off-branch result A") == std::string::npos,
                    "earlier stray does not duplicate a later direct result");
                ok &= check(text.find(expected) < text.find("Parallel continuation"),
                    "recovered result precedes the selected continuation");
            }
        }
    }
    write(source, body +
        R"({"type":"system","subtype":"compact_boundary","uuid":"boundary","parentUuid":null,"logicalParentUuid":"answer","sessionId":"session-a"})" "\n"
        R"({"type":"user","uuid":"summary","parentUuid":"boundary","sessionId":"session-a","isCompactSummary":true,"message":{"role":"user","content":"Retained compact summary"}})" "\n"
        R"({"type":"assistant","uuid":"continued","parentUuid":"summary","sessionId":"session-a","message":{"id":"after-compact","role":"assistant","content":[{"type":"text","text":"After compaction"}]}})" "\n");
    addon::Contract_failure failure;
    for (const auto format : {addon::Conversation_export_format::MARKDOWN, addon::Conversation_export_format::TEXT,
             addon::Conversation_export_format::CONVERSATION})
    {
        for (const bool compact : {false, true}) {
            request.format = format;
            request.compact = compact;
            request.output_directory = addon::path_to_utf8(root / "compacted" / (compact ? "compact" : "full") /
                addon::conversation_export_format_name(format));
            fs::create_directories(addon::path_from_utf8(request.output_directory));
            const auto summary = claude::export_conversation(request, {}, &failure);
            ok &= check(summary && summary->message_count == 2, "compacted branch follows native parent chain");
            if (summary) {
                const auto text = read(addon::path_from_utf8(request.output_directory) / summary->file_name);
                ok &= check(text.find("Compacted conversation summary") != std::string::npos &&
                    text.find("Inherited user prompt") == std::string::npos,
                    "summary identified without duplicated logical ancestry");
                ok &= check(text.find("original messages before compaction are not reconstructed") != std::string::npos,
                    "both projections explain that pre-compaction originals are not reconstructed");
                ok &= check((text.find("Retained compact summary") == std::string::npos) == compact &&
                    text.find("After compaction") != std::string::npos,
                    "compact omits summary text while retaining the following conversation");
                ok &= check((text.find("This export includes that summary") == std::string::npos) == compact,
                    "the compact notice does not claim to include the omitted summary body");
            }
        }
    }
    request.compact = false;
    ok &= compact_projection(root, source, request);
    ok &= advisor_results(root, source, request);
    ok &= empty_projection(root, source, request);
    ok &= malformed_empty_results(root, source, request);
    request.output_directory = addon::path_to_utf8(root / "failures");
    fs::create_directories(addon::path_from_utf8(request.output_directory));
    write(source, R"({"type":"user","uuid":"a","parentUuid":"missing","sessionId":"session-a","message":{"role":"user","content":"orphan"}})" "\n");
    ok &= check(!claude::export_conversation(request, {}, &failure) &&
        failure.reason_code == "conversation_branch_incomplete", "missing parent never silently truncates history");
    write(source, R"({"type":"user","uuid":"a","parentUuid":null,"sessionId":"other","message":{"role":"user","content":"wrong"}})" "\n");
    ok &= check(!claude::export_conversation(request, {}, &failure) &&
        failure.reason_code == "conversation_identity_mismatch", "native session identity checked");
    write(source, body);
    write(profile / "projects/project-b/session-a.jsonl", body);
    ok &= check(!claude::export_conversation(request, {}, &failure) &&
        failure.reason_code == "conversation_source_ambiguous", "ambiguous source is not guessed");
    fs::remove_all(root);
    return ok ? 0 : 1;
}
