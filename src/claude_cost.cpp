#include "claude_cost.h"
#include "claude_accounts.h"
#include "claude_support.h"

#include <logonomic_cli_addon/json.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace claude {
namespace {

namespace addon = logonomic::cli_addon;
namespace fs    = std::filesystem;
using Json      = addon::Json_value;

constexpr std::size_t k_chunk_bytes       = 64 * 1024;
constexpr std::size_t k_max_record_bytes  = 512 * 1024 * 1024;
constexpr std::size_t k_max_receipt_bytes = 64 * 1024;

struct fingerprint_t
{
    std::uintmax_t     size;
    fs::file_time_type modified;

    bool operator==(const fingerprint_t&) const = default;
};

struct Saved_cost
{
    fingerprint_t         fingerprint;
    std::string           session;
    std::optional<double> amount;
    bool                  inherited = false;
};

// The adapter remains alive across probes. File fingerprints avoid repeatedly
// parsing the profile's full transcript history for the account indicator.
std::mutex s_cache_mutex;
std::map<fs::path, Saved_cost> s_transcript_cache;

void stop_if_cancelled(const addon::Invocation_context& context)
{
    if (context.cancel_requested()) {
        throw std::runtime_error("cancelled");
    }
}

fingerprint_t fingerprint(const fs::path& path)
{
    return {fs::file_size(path), fs::last_write_time(path)};
}

double amount(const Json* value)
{
    if (!value || !value->is_number() || !std::isfinite(value->as_number()) || value->as_number() < 0) {
        throw std::runtime_error("cost_amount_invalid");
    }
    return value->as_number();
}

Json object(std::string_view bytes, std::size_t maximum)
{
    addon::Json_parse_limits limits;
    limits.max_bytes        = maximum;
    limits.max_string_bytes = maximum;
    auto value = addon::parse_json(bytes, limits, nullptr);
    if (!value || !value->is_object()) {
        throw std::runtime_error("cost_record_invalid");
    }
    return std::move(*value);
}

void accept_native_record(Saved_cost& saved, std::string_view bytes)
{
    const auto first = bytes.find_first_not_of('\0');
    if (first == std::string_view::npos) {
        return;
    }
    const auto record = object(bytes.substr(first), k_max_record_bytes);
    const auto kind   = text(record, "type");
    const auto owner  = text(record, "sessionId");
    if (record.find("forkedFrom") || kind == "fork-context-ref") {
        saved.inherited = true;
    }
    if (kind != "cost-state") {
        return;
    }
    // A fork can copy its parent's transcript. That parent's saved cost still
    // belongs to the original session, not to the file receiving the copy.
    if (owner != saved.session) {
        return;
    }
    saved.amount = amount(record.find("totalCostUSD"));
}

Saved_cost read_transcript(const fs::path& path, addon::Invocation_context& context)
{
    Saved_cost saved{fingerprint(path), path_string(path.stem()), std::nullopt};
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("cost_source_unreadable");
    }
    std::string chunk(k_chunk_bytes, '\0');
    std::string record;
    while (file) {
        stop_if_cancelled(context);
        file.read(chunk.data(), (std::streamsize)chunk.size());
        const auto count = (std::size_t)file.gcount();
        std::size_t begin = 0;
        while (begin < count) {
            const auto newline = chunk.find('\n', begin);
            const auto end     = std::min(newline, count);
            if (end - begin > k_max_record_bytes - record.size()) {
                throw std::runtime_error("cost_record_capacity");
            }
            record.append(chunk, begin, end - begin);
            if (newline >= count) {
                break;
            }
            accept_native_record(saved, record);
            record.clear();
            begin = newline + 1;
        }
    }
    if (file.bad()) {
        throw std::runtime_error("cost_source_unreadable");
    }
    // An unfinished final line is still being written. A concurrent rewrite
    // invalidates this read instead of publishing a mixture of snapshots.
    if (fingerprint(path) != saved.fingerprint) {
        throw std::runtime_error("cost_source_changed");
    }
    // Claude restores the parent cost ledger when forking. Its new session's
    // total can therefore include the parent's spend without a saved split.
    if (saved.amount && saved.inherited) {
        throw std::runtime_error("cost_session_overlap");
    }
    return saved;
}

bool source_directory(const fs::path& path)
{
    if (!fs::exists(path)) {
        return false;
    }
    if (!fs::is_directory(path)) {
        throw std::runtime_error("cost_source_unreadable");
    }
    return true;
}

void accept_session(std::map<std::string, double>& sessions, const std::string& session, double value)
{
    const auto [found, inserted] = sessions.emplace(session, value);
    if (!inserted && found->second != value) {
        // Neither a copied transcript's mtime nor a receipt's completion time
        // orders it against an undated snapshot from a resumed native session.
        throw std::runtime_error("cost_session_conflict");
    }
}

void read_native_costs(
    const fs::path& profile, addon::Invocation_context& context, std::map<std::string, double>& sessions)
{
    const auto projects = profile / "projects";
    if (!source_directory(projects)) {
        std::erase_if(s_transcript_cache, [&projects](const auto& cached) {
            const auto relative = cached.first.lexically_relative(projects);
            return !relative.empty() && *relative.begin() != "..";
        });
        return;
    }
    std::set<fs::path> present;
    for (const auto& entry : fs::recursive_directory_iterator(projects)) {
        stop_if_cancelled(context);
        if (!entry.is_regular_file() || entry.path().extension() != ".jsonl") {
            continue;
        }
        const auto path = entry.path().lexically_normal();
        present.insert(path);
        auto found = s_transcript_cache.find(path);
        if (found == s_transcript_cache.end() || found->second.fingerprint != fingerprint(path)) {
            auto saved = read_transcript(path, context);
            found = s_transcript_cache.insert_or_assign(path, std::move(saved)).first;
        }
        if (found->second.amount) {
            accept_session(sessions, found->second.session, *found->second.amount);
        }
    }
    std::erase_if(s_transcript_cache, [&projects, &present](const auto& cached) {
        const auto relative = cached.first.lexically_relative(projects);
        return !relative.empty() && *relative.begin() != ".." && !present.contains(cached.first);
    });
}

void read_receipts(
    const fs::path& profile,
    const addon::Account_context& account,
    addon::Invocation_context& context,
    std::map<std::string, double>& sessions)
{
    const auto directory = profile / "logonomic-cost-receipts";
    if (!source_directory(directory)) {
        return;
    }
    for (const auto& entry : fs::directory_iterator(directory)) {
        stop_if_cancelled(context);
        if (!entry.is_regular_file() || entry.path().extension() != ".json") {
            continue;
        }
        const auto before = fingerprint(entry.path());
        if (before.size > k_max_receipt_bytes) {
            throw std::runtime_error("cost_receipt_capacity");
        }
        std::ifstream file(entry.path(), std::ios::binary);
        std::string bytes((std::size_t)before.size, '\0');
        if (!file || !file.read(bytes.data(), (std::streamsize)bytes.size())) {
            throw std::runtime_error("cost_source_unreadable");
        }
        if (fingerprint(entry.path()) != before) {
            throw std::runtime_error("cost_source_changed");
        }
        const auto record   = object(bytes, k_max_receipt_bytes);
        const auto* version = record.find("version");
        const auto* instant = record.find("recorded_at_ms");
        const auto session  = text(record, "session_id");
        const auto basis    = text(record, "cost_basis");
        const auto selected = addon::path_from_utf8(text(record, "profile_path"));
        if (!version || !version->is_integer() || version->as_integer() != 1 ||
            text(record, "source") != "claude-code-result" || text(record, "billing") != "api" ||
            session.empty() || session != path_string(entry.path().stem()) ||
            !instant || !instant->is_integer() || instant->as_integer() <= 0 ||
            (basis != "list" && basis != "managed" && basis != "unknown" && basis != "mixed"))
        {
            throw std::runtime_error("cost_receipt_invalid");
        }
        if (!selected.is_absolute() || account_key(selected) != account.adapter_account_key) {
            throw std::runtime_error("cost_receipt_profile_mismatch");
        }
        accept_session(sessions, session, amount(record.find("total_cost_usd")));
    }
}

} // namespace

std::optional<addon::Quota_cost> read_recorded_cost(
    const addon::Account_context& account, addon::Invocation_context& context)
{
    const auto profile = profile_for_account(account);
    if (!fs::is_directory(profile)) {
        throw std::runtime_error("cost_profile_unavailable");
    }
    stop_if_cancelled(context);
    std::lock_guard lock(s_cache_mutex);
    stop_if_cancelled(context);
    std::map<std::string, double> sessions;
    std::exception_ptr native_failure;
    try {
        read_native_costs(profile, context, sessions);
    }
    catch (const std::exception&) {
        stop_if_cancelled(context);
        native_failure = std::current_exception();
        sessions.clear();
    }
    read_receipts(profile, account, context, sessions);
    if (sessions.empty()) {
        if (native_failure) {
            std::rethrow_exception(native_failure);
        }
        return std::nullopt;
    }
    double total = 0;
    for (const auto& [session, value] : sessions) {
        (void)session;
        total += value;
        if (!std::isfinite(total)) {
            throw std::runtime_error("cost_amount_invalid");
        }
    }
    stop_if_cancelled(context);
    return addon::Quota_cost{
        total, "USD", true, native_failure ? "Recorded API runs" : "Recorded sessions", std::nullopt};
}

} // namespace claude
