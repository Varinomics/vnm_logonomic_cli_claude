#include "claude_usage.h"
#include "claude_accounts.h"
#include "claude_support.h"

#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/sha256.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace claude {
namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
using Json = addon::Json_value;

constexpr std::uint64_t k_max_exact = 9007199254740991;
// A transcript or cache line is held and parsed whole, so each record is
// bounded. Nothing else is: transcripts are read one line at a time, a scan
// holds each retained message once, and an incremental scan parses only the
// transcripts that changed.
constexpr std::size_t k_max_record_bytes = 512 * 1024 * 1024;
constexpr std::size_t k_read_bytes = 1024 * 1024;
// The cache writes a transcript's messages in lines of about this many
// serialized bytes, far below the record bound, so it stays readable however
// many messages one transcript accumulates.
constexpr std::size_t k_cache_line_bytes = 1024 * 1024;
// claude-message-cache-v1 kept each transcript on one line; v2 continues a
// transcript on consecutive lines. Both are read.
constexpr std::string_view k_cache_format = "claude-message-cache-v2";

Json usage_object(std::string_view bytes)
{
    addon::Json_parse_limits limits;
    limits.max_bytes = k_max_record_bytes;
    limits.max_string_bytes = k_max_record_bytes;
    addon::Json_parse_error failure;
    auto value = addon::parse_json(bytes, limits, &failure);
    if (!value || !value->is_object()) throw std::runtime_error("usage_json_invalid");
    return std::move(*value);
}

// Yields the lines of a file while buffering at most one record. The final
// line is reported unterminated when no newline ends the file, because a
// transcript's last record may still be being written.
class Line_reader
{
public:
    explicit Line_reader(const fs::path& path)
    :
        m_stream(path, std::ios::binary)
    {}

    bool is_open() const { return m_stream.is_open(); }
    bool failed()  const { return m_stream.bad(); }

    // False at the end of the input. The view is valid until the next call.
    bool next(std::string_view& line, bool& terminated)
    {
        for (;;) {
            const auto newline = m_buffer.find('\n', m_searched);
            if (newline != std::string::npos) {
                if (newline - m_begin > k_max_record_bytes) throw std::runtime_error("usage_record_capacity");
                line       = std::string_view(m_buffer).substr(m_begin, newline - m_begin);
                m_begin    = newline + 1;
                m_searched = m_begin;
                terminated = true;
                return true;
            }
            if (m_buffer.size() - m_begin > k_max_record_bytes) throw std::runtime_error("usage_record_capacity");
            if (m_end_of_input) {
                if (m_begin == m_buffer.size()) return false;
                line       = std::string_view(m_buffer).substr(m_begin);
                m_begin    = m_buffer.size();
                m_searched = m_begin;
                terminated = false;
                return true;
            }
            m_buffer.erase(0, m_begin);
            m_begin = 0;
            const auto kept = m_buffer.size();
            m_buffer.resize(kept + k_read_bytes);
            m_stream.read(m_buffer.data() + kept, static_cast<std::streamsize>(k_read_bytes));
            const auto received = static_cast<std::size_t>(m_stream.gcount());
            m_buffer.resize(kept + received);
            m_searched     = kept;
            m_end_of_input = received < k_read_bytes;
        }
    }

private:
    std::ifstream m_stream;
    std::string   m_buffer;
    std::size_t   m_begin        = 0;
    std::size_t   m_searched     = 0;
    bool          m_end_of_input = false;
};

// The size and modification time of a transcript when it was parsed. Ticks of
// fs::file_time_type compare only on the platform that recorded them, which
// is the only one that reads this profile's cache.
struct file_fingerprint_t
{
    std::int64_t size     = 0;
    std::int64_t modified = 0;

    bool operator==(const file_fingerprint_t&) const = default;
};

file_fingerprint_t fingerprint(const fs::path& path)
{
    return {
        static_cast<std::int64_t>(fs::file_size(path)),
        static_cast<std::int64_t>(fs::last_write_time(path).time_since_epoch().count()),
    };
}

struct Message
{
    std::string id;
    std::string session;
    std::string date;
    std::string source;
    std::string cwd;
    std::string model;
    std::array<std::optional<std::uint64_t>, 4> categories;
    std::uint64_t total = 0;
};

struct Snapshot
{
    addon::Observation_scope scope;
    std::string profile;
    std::vector<Usage_session> sessions;
    std::uint64_t touched_ms = 0;
    std::optional<addon::Usage_cache_reference> cache_candidate;
};

std::mutex s_snapshot_mutex;
std::mutex s_cache_mutex;
std::map<std::string, Snapshot> s_snapshots;

void add(std::uint64_t& destination, std::uint64_t value)
{
    if (value > k_max_exact - destination) throw std::runtime_error("usage_overflow");
    destination += value;
}

std::string local_date(const std::string& timestamp)
{
    static const std::regex pattern(R"(^(\d{4})-(\d{2})-(\d{2})[Tt ](\d{2}):(\d{2}):(\d{2})(?:\.\d+)?([Zz]|[+-]\d{2}:?\d{2})?$)");
    std::smatch match;
    if (!std::regex_match(timestamp, match, pattern)) throw std::runtime_error("usage_timestamp_invalid");
    const int year   = std::stoi(match[1].str());
    const int month  = std::stoi(match[2].str());
    const int day    = std::stoi(match[3].str());
    const int hour   = std::stoi(match[4].str());
    const int minute = std::stoi(match[5].str());
    const int second = std::stoi(match[6].str());
    const std::chrono::year_month_day date{std::chrono::year(year), std::chrono::month(static_cast<unsigned>(month)),
        std::chrono::day(static_cast<unsigned>(day))};
    if (!date.ok() || year < 1 || hour > 23 || minute > 59 || second > 59) throw std::runtime_error("usage_timestamp_invalid");
    const auto zone = match[7].str();
    std::time_t time;
    if (zone.empty()) {
        std::tm local{};
        local.tm_year = year - 1900;
        local.tm_mon  = month - 1;
        local.tm_mday = day;
        local.tm_hour = hour;
        local.tm_min  = minute;
        local.tm_sec  = second;
        local.tm_isdst = -1;
        time = std::mktime(&local);
    }
    else {
        int offset = 0;
        if (zone != "Z" && zone != "z") {
            const int offset_hour = std::stoi(zone.substr(1, 2));
            const int offset_minute = std::stoi(zone.substr(zone.size() - 2));
            if (offset_hour > 23 || offset_minute > 59) throw std::runtime_error("usage_timestamp_invalid");
            offset = (offset_hour * 60 + offset_minute) * (zone.front() == '-' ? -1 : 1);
        }
        const auto instant = std::chrono::sys_days(date) + std::chrono::hours(hour) +
            std::chrono::minutes(minute - offset) + std::chrono::seconds(second);
        time = std::chrono::system_clock::to_time_t(instant);
    }
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &time) != 0) throw std::runtime_error("usage_timestamp_invalid");
#else
    if (!localtime_r(&time, &local)) throw std::runtime_error("usage_timestamp_invalid");
#endif
    char output[11]{};
    if (std::strftime(output, sizeof(output), "%Y-%m-%d", &local) != 10) throw std::runtime_error("usage_timestamp_invalid");
    return output;
}

std::optional<std::uint64_t> count(const Json& usage, std::string_view name)
{
    const auto* value = usage.find(name);
    if (!value) return std::nullopt;
    if (!value->is_number()) throw std::runtime_error("usage_counter_invalid");
    // Native transcripts are JSON numbers; only exact nonnegative counts can
    // cross the SDK's integer boundary, irrespective of numeric spelling.
    const double number = value->as_number();
    if (number < 0 || number > static_cast<double>(k_max_exact) || std::floor(number) != number) {
        throw std::runtime_error("usage_counter_invalid");
    }
    return static_cast<std::uint64_t>(number);
}

std::optional<std::string> repository(const std::string& cwd)
{
    if (cwd.empty()) return std::nullopt;
    auto path = addon::path_from_utf8(cwd);
    if (!path.is_absolute()) return std::nullopt;
    std::error_code error;
    if (!fs::is_directory(path, error) || error) return std::nullopt;
    path = path.lexically_normal();
    while (!path.empty()) {
        if (fs::exists(path / ".git", error) && !error) return path_string(path);
        const auto parent = path.parent_path();
        if (parent == path) break;
        path = parent;
    }
    return std::nullopt;
}

// Empty when the transcript changed while it was read: a live session is still
// appending to it, and a later scan reads it at rest.
std::optional<std::vector<Message>> parse_file(
    const fs::path& path, addon::Invocation_context& context, file_fingerprint_t& observed)
{
    observed = fingerprint(path);
    Line_reader file(path);
    if (!file.is_open()) throw std::runtime_error("usage_file_unreadable");
    const auto source = path_string(fs::absolute(path).lexically_normal());
    std::vector<Message> result;
    std::string_view line;
    bool terminated = false;
    while (file.next(line, terminated)) {
        if (context.cancel_requested()) throw std::runtime_error("cancelled");
        if (!terminated) break; // An unterminated record is still being written.
        const auto first = line.find_first_not_of('\0');
        const auto event = usage_object(first == std::string_view::npos ? std::string_view{} : line.substr(first));
        if (text(event, "type") != "assistant") continue;
        const auto* message = event.find("message");
        if (!message || !message->is_object()) continue;
        const auto id = text(*message, "id");
        const auto* stop = message->find("stop_reason");
        if (id.empty() || !stop || stop->is_null()) continue;
        const auto* usage = message->find("usage");
        const Json empty = Json::object();
        const Json& values = usage && usage->is_object() ? *usage : empty;
        Message record;
        record.id      = id;
        record.session = text(event, "sessionId");
        if (record.session.empty()) record.session = path_string(path.stem());
        record.date   = local_date(text(event, "timestamp"));
        record.source = source;
        record.cwd    = text(event, "cwd");
        record.model  = text(*message, "model");
        record.categories = {count(values, "input_tokens"), count(values, "output_tokens"),
            count(values, "cache_read_input_tokens"), count(values, "cache_creation_input_tokens")};
        for (const auto& category : record.categories) {
            if (category) add(record.total, *category);
        }
        result.push_back(std::move(record));
    }
    if (file.failed()) throw std::runtime_error("usage_file_unreadable");
    if (fingerprint(path) != observed) return std::nullopt;
    return result;
}

struct Cached_file
{
    // Absent for evidence adopted from a native seed, which is parsed again
    // whenever its transcript is present.
    std::optional<file_fingerprint_t> fingerprint;
    std::vector<Message>              messages;
};

using File_cache = std::map<std::string, Cached_file>;

// One message ID across every transcript that recorded it: the first retained
// copy and what the other copies agree on.
struct merged_message_t
{
    const Message*                              first;
    std::array<std::optional<std::uint64_t>, 4> categories;
    bool                                        breakdown_conflicted;
    bool                                        repository_conflicted;
};

std::vector<Message> reconcile_messages(
    const std::vector<Message>& retained, std::vector<Message> observed, addon::Invocation_context& context)
{
    std::set<std::string> observed_ids;
    for (const auto& message : observed) {
        if (context.cancel_requested()) throw std::runtime_error("cancelled");
        observed_ids.insert(message.id);
    }
    // A completed observation can correct its own message. Missing or unfinished
    // messages provide no evidence that previously completed usage disappeared.
    for (const auto& message : retained) {
        if (context.cancel_requested()) throw std::runtime_error("cancelled");
        if (!observed_ids.contains(message.id)) {
            observed.push_back(message);
        }
    }
    return observed;
}

std::string cache_path(const fs::path& path)
{
    auto normalized = fs::absolute(path).lexically_normal();
#ifdef _WIN32
    const auto source = normalized.native();
    const int size = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, source.data(),
        static_cast<int>(source.size()), nullptr, 0, nullptr, nullptr, 0);
    if (size <= 0) throw std::runtime_error("usage_cache_path_invalid");
    std::wstring folded(static_cast<std::size_t>(size), L'\0');
    if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, source.data(), static_cast<int>(source.size()),
            folded.data(), size, nullptr, nullptr, 0) != size)
    {
        throw std::runtime_error("usage_cache_path_invalid");
    }
    normalized = fs::path(folded);
#endif
    return path_string(normalized);
}


std::string required_text(const Json& value, std::string_view field)
{
    auto result = text(value, field);
    if (result.empty()) throw std::runtime_error("usage_cache_invalid");
    return result;
}

Message cached_message(const Json& value, bool native)
{
    Message result;
    result.id = required_text(value, native ? "record_id" : "id");
    result.session = required_text(value, native ? "session_id" : "session");
    result.date = required_text(value, native ? "completed_date" : "date");
    if (result.date.size() != 10 || local_date(result.date + "T12:00:00") != result.date) {
        throw std::runtime_error("usage_cache_invalid");
    }
    result.source = required_text(value, native ? "source_path" : "source");
    result.cwd = text(value, native ? "recorded_cwd" : "cwd");
    result.model = text(value, "model");
    const auto total = count(value, native ? "total_tokens" : "total");
    if (!total) throw std::runtime_error("usage_cache_invalid");
    result.total = *total;
    const std::array<std::string_view, 4> names{
        "input_tokens", "output_tokens", "cache_read_input_tokens", "cache_creation_input_tokens",
    };
    if (!native) {
        const auto* categories = value.find("categories");
        if (!categories || !categories->is_object()) throw std::runtime_error("usage_cache_invalid");
        for (std::size_t index = 0; index < names.size(); ++index) {
            result.categories[index] = count(*categories, names[index]);
        }
        return result;
    }
    const auto* capabilities = value.find("capabilities");
    if (capabilities && capabilities->is_object()) {
        if (const auto* identity = capabilities->find("usage-record-identity")) {
            if (count(*identity, "version") != 1) throw std::runtime_error("usage_cache_invalid");
            result.session = required_text(*identity, "legacy_session_key");
        }
    }
    const auto* breakdown = capabilities && capabilities->is_object() ? capabilities->find("token_breakdown") : nullptr;
    if (!breakdown) return result;
    const auto version = count(*breakdown, "version");
    if (version != 1 && version != 2) throw std::runtime_error("usage_cache_invalid");
    for (std::size_t index = 0; index < names.size(); ++index) {
        if (version == 1) {
            result.categories[index] = count(*breakdown, names[index]);
            continue;
        }
        const auto* field = breakdown->find(names[index]);
        if (!field || !field->is_object()) throw std::runtime_error("usage_cache_invalid");
        const auto covered = count(*field, "covered_records");
        const auto represented = count(*field, "total_records");
        const auto known = count(*field, "known_tokens");
        const auto support = text(*field, "support");
        if (!covered || !represented || *covered > 1 || *represented != 1 || flag(*field, "overflow") ||
            (support != "supported" && support != "unknown"))
        {
            throw std::runtime_error("usage_cache_invalid");
        }
        if (*covered == 1) {
            if (!known) throw std::runtime_error("usage_cache_invalid");
            result.categories[index] = known;
        }
        else if (known.value_or(0) != 0) throw std::runtime_error("usage_cache_invalid");
    }
    return result;
}

std::optional<file_fingerprint_t> cached_fingerprint(const Json& wrapper)
{
    const auto* size     = wrapper.find("size");
    const auto* modified = wrapper.find("modified");
    if (!size && !modified) return std::nullopt;
    if (!size || !modified || !size->is_integer() || !modified->is_integer() || size->as_integer() < 0) {
        throw std::runtime_error("usage_cache_invalid");
    }
    return file_fingerprint_t{size->as_integer(), modified->as_integer()};
}

std::string read_cache(const fs::path& path, bool native, File_cache& cache, addon::Invocation_context& context, std::string_view expected_hash = {})
{
    Line_reader stream(path);
    if (!stream.is_open()) throw std::runtime_error("usage_cache_unavailable");
    addon::Sha256 digest;
    std::string receipt;
    bool header_seen = native;
    std::string previous_source;
    std::string_view line;
    bool terminated = false;
    while (stream.next(line, terminated)) {
        if (context.cancel_requested()) throw std::runtime_error("cancelled");
        if (!terminated) throw std::runtime_error("usage_cache_incomplete");
        digest.update(line);
        digest.update("\n");
        const auto wrapper = usage_object(line);
        if (!header_seen) {
            const auto format = text(wrapper, "format");
            if (format != k_cache_format && format != "claude-message-cache-v1") throw std::runtime_error("usage_cache_invalid");
            receipt = text(wrapper, "seed_sha256_hex");
            header_seen = true;
            continue;
        }
        const auto raw_source = required_text(wrapper, "path");
        if (!addon::path_from_utf8(raw_source).is_absolute()) throw std::runtime_error("usage_cache_invalid");
        const auto source = cache_path(addon::path_from_utf8(raw_source));
        const auto* records = wrapper.find("records");
        // Only the first of a transcript's consecutive lines carries its fingerprint.
        const bool continued = !native && source == previous_source;
        const auto fingerprint = native ? std::nullopt : cached_fingerprint(wrapper);
        if (!records || !records->is_array() || (continued ? fingerprint.has_value() : cache.contains(source))) {
            throw std::runtime_error("usage_cache_invalid");
        }
        previous_source = source;
        auto& destination = cache[source];
        if (!continued) destination.fingerprint = fingerprint;
        for (const auto& record : records->as_array()) {
            if (context.cancel_requested()) throw std::runtime_error("cancelled");
            destination.messages.push_back(cached_message(record, native));
        }
    }
    if (stream.failed() || !header_seen) throw std::runtime_error("usage_cache_unavailable");
    if (native && (expected_hash.empty() || addon::to_hex(digest.finish()) != expected_hash)) {
        throw std::runtime_error("usage_seed_digest_mismatch");
    }
    return native ? std::string(expected_hash) : receipt;
}

Json cache_record(const Message& message)
{
    Json value = Json::object();
    value.set("id", Json(message.id));
    value.set("session", Json(message.session));
    value.set("date", Json(message.date));
    value.set("source", Json(message.source));
    value.set("cwd", Json(message.cwd));
    value.set("model", Json(message.model));
    value.set("total", Json(message.total));
    Json categories = Json::object();
    const std::array<std::string_view, 4> names{
        "input_tokens", "output_tokens", "cache_read_input_tokens", "cache_creation_input_tokens",
    };
    for (std::size_t index = 0; index < names.size(); ++index) {
        if (message.categories[index]) categories.set(std::string(names[index]), Json(*message.categories[index]));
    }
    value.set("categories", std::move(categories));
    return value;
}

#ifndef _WIN32
void sync_cache_path(const fs::path& path, int flags)
{
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | flags);
    if (descriptor < 0) {
        throw std::runtime_error("usage_cache_write_failed");
    }
    const int synced = ::fsync(descriptor);
    const int closed = ::close(descriptor);
    if (synced != 0 || closed != 0) {
        throw std::runtime_error("usage_cache_write_failed");
    }
}
#endif

void write_cache(const fs::path& path, const File_cache& cache, addon::Invocation_context& context, const std::string& seed_receipt)
{
    const auto temporary = path.parent_path() / ("claude-cache-" + addon::random_nonce_hex() + ".tmp");
    try {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) throw std::runtime_error("usage_cache_write_failed");
        Json header = Json::object();
        header.set("format", Json(k_cache_format));
        header.set("seed_sha256_hex", Json(seed_receipt));
        stream << addon::serialize_json(header) << '\n';
        for (const auto& [source, file] : cache) {
            if (context.cancel_requested()) throw std::runtime_error("cancelled");
            auto next = file.messages.begin();
            bool first = true;
            do {
                Json wrapper = Json::object();
                wrapper.set("path", Json(source));
                if (first && file.fingerprint) {
                    wrapper.set("size",     Json(file.fingerprint->size));
                    wrapper.set("modified", Json(file.fingerprint->modified));
                }
                Json entries = Json::array();
                std::size_t bytes = 0;
                for (; next != file.messages.end() && bytes < k_cache_line_bytes; ++next) {
                    if (context.cancel_requested()) throw std::runtime_error("cancelled");
                    auto entry = cache_record(*next);
                    bytes += addon::serialize_json(entry).size();
                    entries.push_back(std::move(entry));
                }
                wrapper.set("records", std::move(entries));
                stream << addon::serialize_json(wrapper) << '\n';
                first = false;
            }
            while (next != file.messages.end());
        }
        stream.close();
        if (!stream) throw std::runtime_error("usage_cache_write_failed");
        // Replacing the cache commits the scan, so cancellation is checked once
        // more immediately before it: a cancelled scan keeps the cache it
        // started from.
#ifdef _WIN32
        if (context.cancel_requested()) throw std::runtime_error("cancelled");
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::runtime_error("usage_cache_write_failed");
        }
#else
        // The host removes the native seed after success: both its adopted
        // contents and the replacement directory entry must survive power loss.
        sync_cache_path(temporary, 0);
        if (context.cancel_requested()) throw std::runtime_error("cancelled");
        fs::rename(temporary, path);
        sync_cache_path(path.parent_path(), O_DIRECTORY);
#endif
    }
    catch (...) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw;
    }
}

void populate_amounts(Usage_session& session)
{
    const std::array<addon::Usage_amount*, 4> amounts{
        &session.record.input, &session.record.output, &session.record.cache_read, &session.record.cache_creation,
    };
    for (std::size_t index = 0; index < amounts.size(); ++index) {
        const bool complete = session.category_coverage[index] == session.message_count;
        amounts[index]->applicable = true;
        amounts[index]->value = session.category_values[index];
        amounts[index]->coverage = addon::Usage_field_coverage{
            session.category_coverage[index], session.message_count, false,
            complete ? addon::Usage_support::SUPPORTED : addon::Usage_support::UNKNOWN,
        };
    }
    session.record.coverage_unit = addon::Usage_coverage_unit::RECORDS;
    addon::Usage_repository allocation;
    allocation.unattributed_tokens = session.unattributed_tokens;
    for (const auto& [path, tokens] : session.repository_tokens) {
        allocation.allocations.push_back({path, tokens});
    }
    session.record.repository_usage = std::move(allocation);
    session.record.counter_field = "usage.input_tokens + usage.output_tokens + usage.cache_read_input_tokens + usage.cache_creation_input_tokens";
    session.record.source_counter_semantics = "per_event_sum";
}

} // namespace

std::vector<Usage_session> scan_usage(const fs::path& profile, addon::Invocation_context& context, const Usage_cache_input& cache_input)
{
    std::lock_guard cache_lock(s_cache_mutex);
    const auto projects = profile / "projects";
    std::error_code filesystem_error;
    if (!fs::is_directory(profile, filesystem_error) || filesystem_error) {
        throw std::runtime_error("usage_projects_unavailable");
    }
    const bool projects_present = fs::exists(projects, filesystem_error);
    if (filesystem_error) {
        throw std::runtime_error("usage_projects_unavailable");
    }
    if (projects_present && !fs::is_directory(projects, filesystem_error)) {
        throw std::runtime_error("usage_projects_unavailable");
    }
    if (filesystem_error) {
        throw std::runtime_error("usage_projects_unavailable");
    }
    File_cache cache;
    std::string receipt;
    bool cache_loaded = false;
    const auto& input_file = cache_input.selected_file ? cache_input.selected_file : cache_input.storage_file;
    if (input_file && fs::is_regular_file(*input_file)) {
        receipt = read_cache(*input_file, false, cache, context);
        cache_loaded = true;
        if (cache_input.native_seed && receipt != cache_input.seed_sha256_hex) {
            File_cache incoming;
            read_cache(*cache_input.native_seed, true, incoming, context, cache_input.seed_sha256_hex);
            if (!incoming.empty()) throw std::runtime_error("usage_seed_receipt_mismatch");
        }
    }
    else if (cache_input.native_seed) {
        receipt = read_cache(*cache_input.native_seed, true, cache, context, cache_input.seed_sha256_hex);
        cache_loaded = true;
    }
    // Required evidence describes history already retained by the host. A
    // validated empty seed (or a header-only local cache) proves only framing
    // and a receipt, not the message evidence needed to reconstruct that history.
    const bool retained_messages = std::any_of(cache.begin(), cache.end(),
        [](const auto& entry) { return !entry.second.messages.empty(); });
    if (cache_input.cache_required && (!cache_loaded || !retained_messages)) {
        throw std::runtime_error("usage_required_cache_unavailable");
    }
    std::vector<fs::path> files;
    if (projects_present) for (const auto& entry : fs::recursive_directory_iterator(projects)) {
        if (context.cancel_requested()) throw std::runtime_error("cancelled");
        if (entry.is_regular_file() && entry.path().extension() == ".jsonl") files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    // Messages share a few working directories; probe each one once per scan.
    std::map<std::string, std::optional<std::string>> repositories;
    const auto repository_of = [&repositories](const std::string& cwd) -> const std::optional<std::string>& {
        auto found = repositories.find(cwd);
        if (found == repositories.end()) {
            found = repositories.emplace(cwd, repository(cwd)).first;
        }
        return found->second;
    };
    std::uint64_t processed = 0;
    for (const auto& file : files) {
        if (context.cancel_requested()) throw std::runtime_error("cancelled");
        auto& cached = cache[cache_path(file)];
        // Incremental scans keep the evidence of a transcript whose size and
        // modification time still match the ones recorded when it was parsed.
        // A transcript that changes while it is read also keeps its evidence
        // and fingerprint, so the next scan reads it again.
        if (!cache_input.incremental || cached.fingerprint != fingerprint(file)) {
            file_fingerprint_t observed;
            if (auto parsed = parse_file(file, context, observed)) {
                cached.messages    = reconcile_messages(cached.messages, std::move(*parsed), context);
                cached.fingerprint = observed;
            }
        }
        context.report_progress(++processed, files.size());
    }
    // The retained messages stay in the cache; deduplication keys on their IDs
    // and refers to each ID's first copy in the order the cache holds them.
    std::unordered_map<std::string_view, merged_message_t> merged;
    std::vector<const merged_message_t*> order;
    for (const auto& [path, file] : cache) {
        (void)path;
        for (const auto& record : file.messages) {
            if (context.cancel_requested()) throw std::runtime_error("cancelled");
            const auto [previous, inserted] = merged.try_emplace(
                record.id, merged_message_t{&record, record.categories, false, false});
            if (inserted) {
                order.push_back(&previous->second);
                continue;
            }
            auto& existing = previous->second;
            if (existing.first->total != record.total) throw std::runtime_error("conflicting_message_usage");
            if (!existing.breakdown_conflicted) {
                const auto present = [](const auto& categories) {
                    return std::any_of(categories.begin(), categories.end(), [](const auto& value) { return value.has_value(); });
                };
                if (present(existing.categories) && present(record.categories) && existing.categories != record.categories) {
                    existing.categories = {};
                    existing.breakdown_conflicted = true;
                }
                else if (!present(existing.categories) && present(record.categories)) {
                    existing.categories = record.categories;
                }
            }
            if (!existing.repository_conflicted && repository_of(existing.first->cwd) != repository_of(record.cwd)) {
                existing.repository_conflicted = true;
            }
        }
    }
    std::map<std::string, Usage_session> sessions;
    const std::optional<std::string> unattributed;
    for (const auto* entry : order) {
        if (context.cancel_requested()) throw std::runtime_error("cancelled");
        const auto& message = *entry->first;
        auto& session = sessions[message.session];
        session.record.record_id = message.session;
        session.record.session_key = message.session;
        session.record.source_path = message.source;
        const auto previous_model = session.record.model.value_or("");
        session.record.model = previous_model.empty() || previous_model == message.model ? message.model : "multiple";
        session.record.completed_date = std::max(session.record.completed_date, message.date);
        add(session.record.total_tokens, message.total);
        ++session.message_count;
        for (std::size_t index = 0; index < entry->categories.size(); ++index) {
            if (entry->categories[index]) {
                add(session.category_values[index], *entry->categories[index]);
                ++session.category_coverage[index];
            }
        }
        const auto& repo = entry->repository_conflicted ? unattributed : repository_of(message.cwd);
        if (repo) add(session.repository_tokens[*repo], message.total);
        else add(session.unattributed_tokens, message.total);
    }
    std::vector<Usage_session> result;
    for (auto& [id, session] : sessions) {
        if (context.cancel_requested()) throw std::runtime_error("cancelled");
        (void)id;
        populate_amounts(session);
        result.push_back(std::move(session));
    }
    if (cache_input.storage_file) write_cache(*cache_input.storage_file, cache, context, receipt);
    return result;
}

void handle_usage(addon::Invocation_context& context)
{
    if (context.request().operation != "scan") {
        context.reject_unknown_operation();
        return;
    }
    addon::Contract_failure failure;
    const auto version = context.request().capability.version;
    const auto input = addon::decode_usage_scan_request(context.request().payload, &failure, version);
    if (!input) {
        context.fail(addon::Result_status::FAILED, failure.reason_code);
        return;
    }
    const auto profile = profile_for_context(input->context);
    auto profile_key = account_key(profile);
    if (version == 3) {
        auto identity = *input;
        identity.snapshot_id.reset();
        identity.cursor.reset();
        profile_key = addon::to_hex(addon::Sha256::digest(
            addon::serialize_json_canonical(addon::encode_usage_scan_request(identity, 3))));
        const auto& selection = *input->cache_selection;
        const auto& root = input->context.profile_storage->root;
        if (!addon::validate_usage_cache_root(selection.candidate_root, root, &failure)) {
            throw std::runtime_error(failure.reason_code);
        }
        const auto outside_mutable = [&](const std::string& selected) {
            const auto mutable_path = addon::path_from_utf8(root) / "claude-message-cache-v1.jsonl";
            for (auto path = addon::path_from_utf8(selected); path.has_relative_path(); path = path.parent_path()) {
                std::error_code error;
                if (fs::equivalent(path, mutable_path, error)) {
                    return false;
                }
            }
            const auto relative = addon::path_from_utf8(selected).lexically_relative(addon::path_from_utf8(root));
            auto name = addon::path_to_utf8(*relative.begin());
#ifdef _WIN32
            name = addon::ascii_lowercase(name);
#endif
            return name != "claude-message-cache-v1.jsonl" &&
                !(name.starts_with("claude-cache-") && name.ends_with(".tmp"));
        };
        if (!outside_mutable(selection.candidate_root) ||
            (selection.accepted && !outside_mutable(selection.accepted->root)))
        {
            throw std::runtime_error("usage_cache_namespace_conflict");
        }
    }
    std::string snapshot_id;
    std::size_t begin = 0;
    if (!input->snapshot_id) {
        Snapshot snapshot;
        snapshot.profile = profile_key;
        if (!input->context.profile_storage) throw std::runtime_error("profile_storage_unavailable");
        Usage_cache_input cache;
        cache.incremental  = input->mode == "incremental";
        cache.storage_file = addon::path_from_utf8(input->context.profile_storage->root) / "claude-message-cache-v1.jsonl";
        if (version == 3) {
            const auto& selection = *input->cache_selection;
            const auto& root = input->context.profile_storage->root;
            if (!addon::validate_usage_cache_allocation(selection.candidate_root, root, &failure)) {
                throw std::runtime_error(failure.reason_code);
            }
            cache.storage_file = addon::usage_cache_io_path(addon::path_from_utf8(selection.candidate_root)) / "messages.jsonl";
            if (selection.accepted) {
                const auto manifest = addon::validate_usage_cache_candidate(
                    *selection.accepted, root, input->context.account_context,
                    [&context] { return context.cancel_requested(); }, &failure);
                if (!manifest) throw std::runtime_error(failure.reason_code);
                if ((manifest->format != "claude-message-cache@1" && manifest->format != "claude-message-cache@2") ||
                    manifest->files.size() != 1 ||
                    manifest->files.front().path != "messages.jsonl")
                {
                    throw std::runtime_error("usage_cache_format_unsupported");
                }
                cache.selected_file = addon::usage_cache_io_path(addon::path_from_utf8(selection.accepted->root)) / "messages.jsonl";
            }
        }
        if (input->native_file_cache) {
            if (!input->context.account_context || input->native_file_cache->source_account_key !=
                    input->context.account_context->adapter_account_key)
            {
                throw std::runtime_error("usage_seed_account_mismatch");
            }
            cache.native_seed = addon::path_from_utf8(input->native_file_cache->path);
            cache.seed_sha256_hex = input->native_file_cache->sha256_hex;
            cache.cache_required = input->native_file_cache->cache_required;
        }
        snapshot.sessions = scan_usage(profile, context, cache);
        snapshot.scope.observed_at_ms = now_ms();
        snapshot.scope.fresh_for_ms = 60000;
        if (input->context.account_context) snapshot.scope.source_account_key = input->context.account_context->adapter_account_key;
        snapshot.touched_ms = now_ms();
        snapshot_id = addon::random_nonce_hex();
        if (version == 3) {
            // Surviving transcripts do not prove that all contributing messages remain.
            for (auto& session : snapshot.sessions) session.record.aggregate_evidence = "partial";
            snapshot.cache_candidate = addon::seal_usage_cache_candidate(
                input->cache_selection->candidate_root, input->context.profile_storage->root,
                input->context.account_context, snapshot_id, "claude-message-cache@2",
                [&context] { return context.cancel_requested(); }, &failure);
            if (!snapshot.cache_candidate) throw std::runtime_error(failure.reason_code);
        }
        std::lock_guard lock(s_snapshot_mutex);
        std::erase_if(s_snapshots, [](const auto& entry) { return now_ms() - entry.second.touched_ms > 600000; });
        if (s_snapshots.size() >= 8) throw std::runtime_error("usage_snapshot_capacity");
        s_snapshots.emplace(snapshot_id, std::move(snapshot));
    }
    else {
        snapshot_id = *input->snapshot_id;
        const auto& cursor = *input->cursor;
        const auto parsed = std::from_chars(cursor.data(), cursor.data() + cursor.size(), begin);
        if (parsed.ec != std::errc{} || parsed.ptr != cursor.data() + cursor.size() || std::to_string(begin) != cursor || begin == 0) {
            throw std::runtime_error("usage_cursor_invalid");
        }
    }
    addon::Usage_scan_page page;
    page.incomplete_historical_coverage = true;
    {
        std::lock_guard lock(s_snapshot_mutex);
        const auto found = s_snapshots.find(snapshot_id);
        if (found == s_snapshots.end() || found->second.profile != profile_key || begin > found->second.sessions.size()) {
            throw std::runtime_error("usage_snapshot_unavailable");
        }
        auto& snapshot = found->second;
        snapshot.touched_ms = now_ms();
        page.snapshot_id = snapshot_id;
        page.scope = snapshot.scope;
        page.cache_candidate = snapshot.cache_candidate;
        if (version == 3) {
            page.account_context = input->context.account_context;
            page.profile_storage = input->context.profile_storage;
        }
        const auto end = std::min(begin + addon::k_max_usage_page_records, snapshot.sessions.size());
        for (auto index = begin; index < end; ++index) page.records.push_back(snapshot.sessions[index].record);
        page.complete = end == snapshot.sessions.size();
        if (!page.complete) page.next_cursor = std::to_string(end);
        else s_snapshots.erase(found);
    }
    context.succeed(addon::encode_usage_scan_result(page, version));
}

} // namespace claude
