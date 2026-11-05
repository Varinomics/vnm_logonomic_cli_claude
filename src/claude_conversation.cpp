#include "claude_conversation.h"

#include "claude_accounts.h"

#include <logonomic_cli_addon/json_utilities.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/session_contracts.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace claude {
namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
namespace json = addon::json;
using Json = addon::Json_value;
using Block = addon::Conversation_block;
using Kind = addon::Conversation_block_kind;
using Phase = addon::Conversation_message_phase;
using Entries = std::vector<Json>;
using Index = std::map<std::string, std::size_t>;

bool fail(addon::Contract_failure* failure, std::string_view reason, std::string_view detail)
{
    *failure = {std::string(reason), std::string(detail)};
    return false;
}

bool flag(const Json& value, std::string_view key)
{
    const auto& member = json::member(value, key);
    return member.is_boolean() && member.as_boolean();
}

bool visible(const Json& row)
{
    const auto type = json::text(row, "type");
    return (type == "user" || type == "assistant") && !flag(row, "isSidechain") &&
        !flag(row, "isMeta") && json::text(row, "teamName").empty();
}

bool same_writer(const Json& left, const Json& right)
{
    return flag(left, "isSidechain") == flag(right, "isSidechain") &&
        json::text(left, "agentId") == json::text(right, "agentId");
}

std::string message_id(const Json& row)
{
    return json::text(row, "type") == "assistant" ? json::text(json::member(row, "message"), "id") : std::string{};
}

std::vector<std::string> block_ids(const Json& row, std::string_view type, std::string_view id_field)
{
    std::vector<std::string> ids;
    const auto& content = json::member(json::member(row, "message"), "content");
    if (content.is_array()) {
        for (const auto& block : content.as_array()) {
            if (json::text(block, "type") == type) {
                const auto id = json::text(block, id_field);
                if (!id.empty()) {
                    ids.push_back(id);
                }
            }
        }
    }
    return ids;
}

std::optional<fs::path> source_file(const fs::path& profile, std::string_view id,
    const std::function<bool()>& cancelled, addon::Contract_failure* failure)
{
    const auto root = profile / "projects";
    std::optional<fs::path> source;
    // The request decoder validates text; lookup must also stay within one
    // filename component, including on Windows (drive and stream syntax).
    if (id.find_first_of("/\\:") == std::string_view::npos && fs::is_directory(root)) {
        const auto name = addon::path_from_utf8(std::string(id) + ".jsonl");
        for (const auto& project : fs::directory_iterator(root)) {
            if (cancelled && cancelled()) {
                fail(failure, "cancelled", "Conversation extraction was cancelled.");
                return {};
            }
            if (!project.is_directory()) {
                continue;
            }
            // Only the project's own session file is authoritative. Nested
            // subagent files may carry the same sessionId as the parent.
            const auto candidate = project.path() / name;
            if (!fs::is_regular_file(candidate)) {
                continue;
            }
            if (source) {
                fail(failure, "conversation_source_ambiguous",
                    "Several native files name this Claude conversation.");
                return {};
            }
            source = candidate;
        }
    }
    if (!source) {
        fail(failure, "conversation_source_unavailable", "The native Claude conversation file is unavailable.");
    }
    return source;
}

std::optional<std::vector<std::size_t>> active_chain(
    const Entries& entries, const Index& by_id, addon::Contract_failure* failure)
{
    std::set<std::string> parents;
    for (const auto& [id, index] : by_id) {
        const auto parent = json::text(entries[index], "parentUuid");
        if (!parent.empty()) {
            parents.insert(parent);
        }
    }
    std::optional<std::size_t> leaf;
    for (const auto& [id, index] : by_id) {
        if (parents.contains(id)) {
            continue;
        }
        auto current = by_id.find(id);
        std::set<std::string> visited;
        while (current != by_id.end() && visited.insert(current->first).second) {
            const auto& row = entries[current->second];
            if (visible(row)) {
                if (!leaf || current->second > *leaf) {
                    leaf = current->second;
                }
                break;
            }
            current = by_id.find(json::text(row, "parentUuid"));
        }
    }
    if (!leaf) {
        fail(failure, "conversation_content_unavailable", "No retained Claude conversation messages are available.");
        return {};
    }
    std::vector<std::size_t> chain;
    std::set<std::string> visited;
    auto id = json::text(entries[*leaf], "uuid");
    while (!id.empty()) {
        const auto item = by_id.find(id);
        if (item == by_id.end() || !visited.insert(id).second) {
            fail(failure, "conversation_branch_incomplete",
                "The retained Claude conversation has a broken parent chain.");
            return {};
        }
        chain.push_back(item->second);
        id = json::text(entries[item->second], "parentUuid");
    }
    std::reverse(chain.begin(), chain.end());
    return chain;
}

std::vector<std::size_t> restore_parallel_tools(
    const Entries& entries, const Index& by_id, const std::vector<std::size_t>& chain)
{
    std::set<std::size_t> selected(chain.begin(), chain.end());
    std::map<std::string, std::size_t> anchors;
    std::map<std::string, std::vector<std::size_t>> groups;
    std::map<std::string, std::string> call_owner;
    std::set<std::string> answered_on_chain;
    for (const auto index : chain) {
        const auto id = message_id(entries[index]);
        if (!id.empty()) {
            anchors[id] = index;
        }
        for (const auto& call : block_ids(entries[index], "tool_result", "tool_use_id")) {
            answered_on_chain.insert(call);
        }
    }
    for (const auto& [uuid, index] : by_id) {
        const auto id = message_id(entries[index]);
        if (!id.empty()) {
            groups[id].push_back(index);
            for (const auto& call : block_ids(entries[index], "tool_use", "id")) {
                const auto [owner, inserted] = call_owner.emplace(call, id);
                if (!inserted && owner->second != id) {
                    owner->second.clear();
                }
            }
        }
    }
    std::map<std::string, std::vector<std::size_t>> results_by_response;
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& row = entries[index];
        if (json::text(row, "type") != "user" || by_id.at(json::text(row, "uuid")) != index) {
            continue;
        }
        const auto calls = block_ids(row, "tool_result", "tool_use_id");
        if (calls.empty()) {
            continue;
        }
        std::set<std::string> responses;
        for (const auto* field : {"parentUuid", "sourceToolAssistantUUID"}) {
            const auto parent = by_id.find(json::text(row, field));
            if (parent != by_id.end() && same_writer(row, entries[parent->second])) {
                const auto response = message_id(entries[parent->second]);
                if (!response.empty()) {
                    responses.insert(response);
                }
            }
        }
        for (const auto& call : calls) {
            const auto owner = call_owner.find(call);
            if (owner != call_owner.end() && !owner->second.empty()) {
                responses.insert(owner->second);
            }
        }
        for (const auto& response : responses) {
            results_by_response[response].push_back(index);
        }
    }
    std::map<std::size_t, std::vector<std::size_t>> inserts;
    for (const auto& [id, anchor] : anchors) {
        std::set<std::string> member_ids;
        std::vector<std::size_t> siblings;
        std::vector<std::size_t> results;
        std::vector<std::size_t> strays;
        auto answered = answered_on_chain;
        for (const auto index : groups[id]) {
            if (!same_writer(entries[index], entries[anchor])) {
                continue;
            }
            member_ids.insert(json::text(entries[index], "uuid"));
            if (!selected.contains(index)) {
                siblings.push_back(index);
            }
        }
        // Parallel tool results point to their own assistant content block,
        // so a plain parent walk omits valid siblings from the same response.
        for (const auto index : results_by_response[id]) {
            const auto& row = entries[index];
            if (selected.contains(index) || !same_writer(row, entries[anchor])) {
                continue;
            }
            const auto calls = block_ids(row, "tool_result", "tool_use_id");
            if (calls.empty()) {
                continue;
            }
            if (member_ids.contains(json::text(row, "parentUuid"))) {
                results.push_back(index);
                answered.insert(calls.begin(), calls.end());
                selected.insert(index);
            }
            else {
                strays.push_back(index);
            }
        }
        // Native direct-parent results take precedence over off-branch results,
        // including strays written earlier. Only then recover unanswered calls.
        for (const auto index : strays) {
            const auto calls = block_ids(entries[index], "tool_result", "tool_use_id");
            const bool open_call = std::any_of(calls.begin(), calls.end(), [&](const auto& call) {
                const auto owner = call_owner.find(call);
                return !answered.contains(call) && owner != call_owner.end() && owner->second == id;
            });
            if (!open_call) {
                continue;
            }
            results.push_back(index);
            answered.insert(calls.begin(), calls.end());
            selected.insert(index);
        }
        const auto chronological = [&](std::size_t left, std::size_t right) {
            const auto a = json::text(entries[left], "timestamp");
            const auto b = json::text(entries[right], "timestamp");
            return a == b ? left < right : a < b;
        };
        std::sort(siblings.begin(), siblings.end(), chronological);
        std::sort(results.begin(), results.end(), chronological);
        siblings.insert(siblings.end(), results.begin(), results.end());
        selected.insert(siblings.begin(), siblings.end());
        inserts[anchor] = std::move(siblings);
    }
    std::vector<std::size_t> restored;
    for (const auto index : chain) {
        restored.push_back(index);
        const auto found = inserts.find(index);
        if (found != inserts.end()) {
            restored.insert(restored.end(), found->second.begin(), found->second.end());
        }
    }
    return restored;
}

bool content_blocks(const Json& content, std::vector<Block>& blocks, Kind kind,
    std::string_view label, bool compact_visible, addon::Contract_failure* failure)
{
    if (content.is_string()) {
        blocks.push_back({kind, content.as_string(), std::string(label), compact_visible});
        return true;
    }
    if (!content.is_array()) {
        return fail(failure, "conversation_content_invalid", "A Claude message has invalid content.");
    }
    for (const auto& part : content.as_array()) {
        const auto type = json::text(part, "type");
        if (type == "text" || type == "thinking") {
            const auto& value = json::member(part, type == "thinking" ? "thinking" : "text");
            if (!value.is_string()) {
                return fail(failure, "conversation_content_invalid", "A Claude text block is incomplete.");
            }
            if (type == "thinking" && value.as_string().empty()) {
                continue;
            }
            blocks.push_back({kind, value.as_string(), type == "thinking" ? "Thinking" : std::string(label),
                compact_visible && type != "thinking"});
        }
        else
        if (type == "tool_reference") {
            const auto& name = json::member(part, "tool_name");
            if (!name.is_string() || name.as_string().empty()) {
                return fail(failure, "conversation_content_invalid", "A Claude tool reference is incomplete.");
            }
            blocks.push_back({kind, addon::serialize_json(part), std::string(label), false});
        }
        else
        if (type == "redacted_thinking") {
            blocks.push_back({Kind::TEXT, "[Encrypted message]", {}, true});
        }
        else
        if (type == "tool_use" || type == "server_tool_use") {
            blocks.push_back({Kind::TOOL_CALL, addon::serialize_json(json::member(part, "input")),
                json::text(part, "name") + " " + json::text(part, "id"), false});
        }
        else
        if (type == "tool_result") {
            const auto call = json::text(part, "tool_use_id");
            if (call.empty()) {
                return fail(failure, "conversation_content_invalid", "A Claude tool result has no call identity.");
            }
            const auto* result = part.find("content");
            if (result && !content_blocks(*result, blocks, Kind::TOOL_RESULT,
                    call, false, failure))
            {
                return false;
            }
        }
        else
        if (type == "advisor_tool_result") {
            const auto& result = json::member(part, "content");
            const auto& result_type = json::member(result, "type");
            const auto call = json::text(part, "tool_use_id");
            if (call.empty() || !result_type.is_string()) {
                return fail(failure, "conversation_content_invalid", "A Claude advisor result is incomplete.");
            }
            if (result_type.as_string() == "advisor_result") {
                const auto& text = json::member(result, "text");
                if (!text.is_string()) {
                    return fail(failure, "conversation_content_invalid", "A Claude advisor result is incomplete.");
                }
                blocks.push_back({Kind::TOOL_RESULT, text.as_string(), call, false});
            }
            else
            if (result_type.as_string() == "advisor_redacted_result") {
                if (!json::member(result, "encrypted_content").is_string()) {
                    return fail(failure, "conversation_content_invalid", "A Claude advisor result is incomplete.");
                }
                blocks.push_back({Kind::TEXT, "[Encrypted message]", {}, true});
            }
            else
            if (result_type.as_string() == "advisor_tool_result_error") {
                if (json::text(result, "error_code").empty()) {
                    return fail(failure, "conversation_content_invalid", "A Claude advisor result is incomplete.");
                }
                blocks.push_back({Kind::TOOL_RESULT, addon::serialize_json(result), call, false});
            }
            else {
                return fail(failure, "conversation_content_unavailable", "A Claude advisor result is unsupported.");
            }
        }
        else
        if (type == "image" || type == "document") {
            const auto& source = json::member(part, "source");
            auto ref = json::text(source, "url");
            if (ref.empty()) {
                ref = json::text(source, "file_id");
            }
            if (ref.empty()) {
                ref = json::text(source, "type") == "base64"
                    ? "Embedded " + json::text(source, "media_type") + " attachment" : json::text(source, "data");
            }
            if (ref.empty()) {
                return fail(failure, "conversation_content_unavailable", "A Claude attachment source is unsupported.");
            }
            blocks.push_back({Kind::ATTACHMENT, std::move(ref), json::text(part, "title"), compact_visible});
        }
        else {
            return fail(failure, "conversation_content_unavailable",
                "A Claude content block has an unsupported format.");
        }
    }
    return true;
}

} // namespace

std::optional<addon::Conversation_export_result> export_conversation(
    const addon::Conversation_export_request& request, const std::function<bool()>& cancelled,
    addon::Contract_failure* failure)
{
    addon::Session_launch_context context;
    context.launch_environment = request.launch_environment;
    context.account_context = request.account_context;
    context.working_directory = request.working_directory;
    const auto source = source_file(profile_for_context(context), request.provider_session_id, cancelled, failure);
    if (!source) {
        return {};
    }
    Entries entries;
    Index by_id;
    const auto snapshot = addon::read_conversation_jsonl(*source,
        [&](const Json& row, addon::Contract_failure* error) {
            const auto type = json::text(row, "type");
            const auto id = json::text(row, "uuid");
            if (type != "user" && type != "assistant" && type != "system" &&
                type != "progress" && type != "attachment")
            {
                return true;
            }
            if (id.empty()) {
                return fail(error, "conversation_content_invalid", "A Claude transcript entry has no identity.");
            }
            if ((type == "user" || type == "assistant") &&
                json::text(row, "sessionId") != request.provider_session_id)
            {
                return fail(error, "conversation_identity_mismatch", "The Claude file contains another conversation.");
            }
            by_id[id] = entries.size();
            entries.push_back(row);
            return true;
        }, cancelled, failure);
    if (!snapshot) {
        return {};
    }
    const auto chain = active_chain(entries, by_id, failure);
    if (!chain) {
        return {};
    }
    const auto restored = restore_parallel_tools(entries, by_id, *chain);
    addon::Conversation_writer writer(request, cancelled);
    if (!writer.open(failure)) {
        return {};
    }
    for (const auto index : restored) {
        const auto& row = entries[index];
        if (!visible(row)) {
            continue;
        }
        addon::Conversation_message message;
        if (flag(row, "isCompactSummary")) {
            message.role = "Compacted conversation summary";
            message.content.push_back({Kind::TEXT,
                request.compact
                    ? "Claude retained a summary of earlier messages; original messages before compaction "
                      "are not reconstructed."
                    : "Claude retained a summary of earlier messages. This export includes that summary and the "
                      "following conversation; original messages before compaction are not reconstructed.", {}, true});
        }
        else {
            message.role = json::text(row, "type");
        }
        if (message.role == "assistant") {
            const auto stop = json::text(json::member(row, "message"), "stop_reason");
            message.phase = stop == "end_turn" ? Phase::FINAL
                : stop == "tool_use" || stop == "pause_turn" ? Phase::COMMENTARY : Phase::UNKNOWN;
        }
        const bool compact_visible = !flag(row, "isCompactSummary") && message.phase != Phase::COMMENTARY;
        if (!content_blocks(json::member(json::member(row, "message"), "content"), message.content,
                Kind::TEXT, {}, compact_visible, failure))
        {
            return {};
        }
        if (!message.content.empty() && !writer.append(message, failure)) {
            return {};
        }
    }
    return writer.finish(snapshot->incomplete_tail_omitted, failure);
}

} // namespace claude
