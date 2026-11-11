#include "claude_usage.h"
#include "claude_accounts.h"
#include "claude_filesystem.h"
#include "claude_support.h"

#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/sha256.h>
#include <logonomic_cli_addon/usage_incremental.h>

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
#include <new>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
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
// A transcript continues on consecutive bounded cache lines.
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
        m_stream(native_io_path(path), std::ios::binary)
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
    const auto io_path = native_io_path(path);
    return {
        static_cast<std::int64_t>(fs::file_size(io_path)),
        static_cast<std::int64_t>(fs::last_write_time(io_path).time_since_epoch().count()),
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
    std::optional<std::string> instant;
    std::optional<Json> temporal_carrier;
    bool temporal_unsupported = false;
    bool temporal_retained = false;
    std::optional<std::string> owner_session;
};

Json cache_accounting_record(const Message& message);

bool valid_message_instant(const std::string& instant)
{
    if (instant.size() > addon::k_max_usage_temporal_bytes) {
        return false;
    }
    addon::Usage_temporal proof;
    proof.parent = {"message", 1, "native-observation", std::nullopt};
    proof.portions.push_back({"instant", instant, {}, {}, 1});
    return addon::decode_usage_temporal(addon::encode_usage_temporal(proof), nullptr).has_value();
}

std::string message_accounting_digest(const Message& message)
{
    return addon::to_hex(addon::Sha256::digest(
        addon::serialize_json_canonical(cache_accounting_record(message))));
}

void read_message_temporal(Message& message, const Json& carrier)
{
    message.temporal_carrier = carrier;
    message.temporal_unsupported = true;
    const auto* version = carrier.find("version");
    const auto* digest = carrier.find("parent_sha256_hex");
    const auto* instant = carrier.find("instant");
    if (!carrier.is_object() || carrier.as_object().size() != 3 ||
        !version || !version->is_integer() || version->as_integer() != 1 ||
        !digest || !digest->is_string() || digest->as_string() != message_accounting_digest(message) ||
        !instant || !instant->is_string() || !valid_message_instant(instant->as_string()))
    {
        return;
    }
    message.instant = instant->as_string();
    message.temporal_unsupported = false;
    message.temporal_retained = true;
}

struct Snapshot
{
    addon::Observation_scope scope;
    std::string profile;
    std::vector<Usage_session> sessions;
    std::uint64_t touched_ms = 0;
    std::optional<addon::Usage_cache_reference> cache_candidate;
};

struct Indexed_source
{
    std::optional<file_fingerprint_t> fingerprint;
    std::vector<std::pair<std::string, std::string>> messages;
    std::set<std::string> directories;
    std::map<std::string, std::string> owners;
};

using Repository_map = std::map<std::string, std::optional<std::string>>;

struct Usage_index
{
    std::string seed_receipt;
    std::map<std::string, Indexed_source> sources;
    Repository_map repositories;
};

constexpr std::string_view k_index_format = "claude-message-pieces@1";
constexpr const char* k_index_file = "index.jsonl";

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
    if (!fs::is_directory(native_io_path(path), error) || error) return std::nullopt;
    path = path.lexically_normal();
    while (!path.empty()) {
        if (fs::exists(native_io_path(path / ".git"), error) && !error) return path_string(path);
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
        const auto timestamp = text(event, "timestamp");
        record.date   = local_date(timestamp);
        record.source = source;
        record.cwd    = text(event, "cwd");
        record.model  = text(*message, "model");
        record.categories = {count(values, "input_tokens"), count(values, "output_tokens"),
            count(values, "cache_read_input_tokens"), count(values, "cache_creation_input_tokens")};
        for (const auto& category : record.categories) {
            if (category) add(record.total, *category);
        }
        if (valid_message_instant(timestamp)) {
            record.instant = timestamp;
        }
        else {
            record.temporal_unsupported = true;
        }
        result.push_back(std::move(record));
    }
    if (file.failed()) throw std::runtime_error("usage_file_unreadable");
    if (fingerprint(path) != observed) return std::nullopt;
    return result;
}

struct Cached_file
{
    // A record without a saved fingerprint is parsed again whenever its
    // transcript is present.
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
    std::optional<std::string>                   instant;
    bool                                        temporal_conflicted;
    bool                                        temporal_unsupported;
    bool                                        temporal_retained;
};

std::vector<Message> reconcile_messages(
    const std::vector<Message>& retained, std::vector<Message> observed, addon::Invocation_context& context)
{
    std::set<std::string> observed_ids;
    std::map<std::string, std::string> retained_owners;
    for (const auto& message : retained) {
        if (message.owner_session) {
            retained_owners.emplace(message.id, *message.owner_session);
        }
    }
    for (auto& message : observed) {
        if (context.cancel_requested()) throw std::runtime_error("cancelled");
        if (observed_ids.insert(message.id).second) {
            const auto owner = retained_owners.find(message.id);
            if (owner != retained_owners.end()) {
                message.owner_session = owner->second;
            }
        }
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
    const auto& source = normalized.native();
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

Message cached_message(const Json& value)
{
    Message result;
    result.id = required_text(value, "id");
    result.session = required_text(value, "session");
    result.date = required_text(value, "date");
    if (result.date.size() != 10 || local_date(result.date + "T12:00:00") != result.date) {
        throw std::runtime_error("usage_cache_invalid");
    }
    result.source = required_text(value, "source");
    result.cwd = text(value, "cwd");
    result.model = text(value, "model");
    const auto total = count(value, "total");
    if (!total) throw std::runtime_error("usage_cache_invalid");
    result.total = *total;
    const std::array<std::string_view, 4> names{
        "input_tokens", "output_tokens", "cache_read_input_tokens", "cache_creation_input_tokens",
    };
    if (const auto* owner = value.find("owner_session")) {
        if (!owner->is_string() || owner->as_string().empty()) {
            throw std::runtime_error("usage_cache_invalid");
        }
        result.owner_session = owner->as_string();
    }
    const auto* categories = value.find("categories");
    if (!categories || !categories->is_object()) throw std::runtime_error("usage_cache_invalid");
    for (std::size_t index = 0; index < names.size(); ++index) {
        result.categories[index] = count(*categories, names[index]);
    }
    if (const auto* temporal = value.find("usage-temporal")) {
        read_message_temporal(result, *temporal);
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

std::string read_cache(const fs::path& path, File_cache& cache, addon::Invocation_context& context)
{
    Line_reader stream(path);
    if (!stream.is_open()) throw std::runtime_error("usage_cache_unavailable");
    std::string receipt;
    bool header_seen = false;
    std::string previous_source;
    std::string_view line;
    bool terminated = false;
    while (stream.next(line, terminated)) {
        if (context.cancel_requested()) throw std::runtime_error("cancelled");
        if (!terminated) throw std::runtime_error("usage_cache_incomplete");
        const auto wrapper = usage_object(line);
        if (!header_seen) {
            const auto format = text(wrapper, "format");
            if (format != k_cache_format) throw std::runtime_error("usage_cache_invalid");
            receipt = text(wrapper, "seed_sha256_hex");
            header_seen = true;
            continue;
        }
        const auto raw_source = required_text(wrapper, "path");
        if (!addon::path_from_utf8(raw_source).is_absolute()) throw std::runtime_error("usage_cache_invalid");
        const auto source = cache_path(addon::path_from_utf8(raw_source));
        const auto* records = wrapper.find("records");
        // Only the first of a transcript's consecutive lines carries its fingerprint.
        const bool continued = source == previous_source;
        const auto fingerprint = cached_fingerprint(wrapper);
        if (!records || !records->is_array() || (continued ? fingerprint.has_value() : cache.contains(source))) {
            throw std::runtime_error("usage_cache_invalid");
        }
        previous_source = source;
        auto& destination = cache[source];
        if (!continued) destination.fingerprint = fingerprint;
        for (const auto& record : records->as_array()) {
            if (context.cancel_requested()) throw std::runtime_error("cancelled");
            destination.messages.push_back(cached_message(record));
        }
    }
    if (stream.failed() || !header_seen) throw std::runtime_error("usage_cache_unavailable");
    return receipt;
}

Json cache_accounting_record(const Message& message)
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

Json cache_record(const Message& message)
{
    auto value = cache_accounting_record(message);
    if (message.owner_session) {
        value.set("owner_session", Json(*message.owner_session));
    }
    if (message.temporal_carrier) {
        value.set("usage-temporal", *message.temporal_carrier);
    }
    else
    if (message.instant) {
        Json carrier = Json::object();
        carrier.set("version", Json(1));
        carrier.set("parent_sha256_hex", Json(message_accounting_digest(message)));
        carrier.set("instant", Json(*message.instant));
        value.set("usage-temporal", std::move(carrier));
    }
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
        // Both the cache contents and the replacement directory entry must
        // survive power loss.
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

std::string source_piece(const std::string& source)
{
    return "source-" + addon::to_hex(addon::Sha256::digest(source)) + ".jsonl";
}

Indexed_source index_source(const Cached_file& source)
{
    Indexed_source indexed;
    indexed.fingerprint = source.fingerprint;
    for (const auto& message : source.messages) {
        indexed.messages.emplace_back(message.id, message.session);
        indexed.directories.insert(message.cwd);
        if (message.owner_session && !indexed.owners.emplace(message.id, *message.owner_session).second) {
            throw std::runtime_error("usage_cache_invalid");
        }
    }
    return indexed;
}

Usage_index read_index(const fs::path& root, addon::Invocation_context& context)
{
    Line_reader stream(root / k_index_file);
    if (!stream.is_open()) {
        throw std::runtime_error("usage_cache_unavailable");
    }
    Usage_index result;
    std::string_view line;
    bool terminated = false;
    bool header = false;
    std::string previous_source;
    while (stream.next(line, terminated)) {
        if (context.cancel_requested()) {
            throw std::runtime_error("cancelled");
        }
        if (!terminated) {
            throw std::runtime_error("usage_cache_incomplete");
        }
        const auto value = usage_object(line);
        if (!header) {
            if (text(value, "format") != k_index_format) {
                throw std::runtime_error("usage_cache_format_unsupported");
            }
            result.seed_receipt = text(value, "seed_sha256_hex");
            header = true;
            continue;
        }
        if (const auto* cwd = value.find("cwd")) {
            const auto* path = value.find("repository");
            if (!previous_source.empty() || !cwd->is_string() || !path ||
                (!path->is_null() && !path->is_string()) ||
                !result.repositories.emplace(cwd->as_string(), path->is_null() ?
                    std::nullopt : std::optional<std::string>(path->as_string())).second)
            {
                throw std::runtime_error("usage_cache_invalid");
            }
            continue;
        }
        const auto source = required_text(value, "path");
        const auto* messages = value.find("messages");
        const bool continued = source == previous_source;
        if (!addon::path_from_utf8(source).is_absolute() ||
            cache_path(addon::path_from_utf8(source)) != source ||
            !messages || !messages->is_array() || (!continued && result.sources.contains(source)))
        {
            throw std::runtime_error("usage_cache_invalid");
        }
        auto& indexed = result.sources[source];
        const auto observed = cached_fingerprint(value);
        if (continued && observed) {
            throw std::runtime_error("usage_cache_invalid");
        }
        if (!continued) {
            indexed.fingerprint = observed;
        }
        previous_source = source;
        const auto* directories = value.find("directories");
        const auto* owners = value.find("owners");
        if (!directories || !directories->is_array() || !owners || !owners->is_array()) {
            throw std::runtime_error("usage_cache_invalid");
        }
        for (const auto& cwd : directories->as_array()) {
            if (!cwd.is_string() || !result.repositories.contains(cwd.as_string()) ||
                !indexed.directories.insert(cwd.as_string()).second)
            {
                throw std::runtime_error("usage_cache_invalid");
            }
        }
        for (const auto& owner : owners->as_array()) {
            if (!owner.is_array() || owner.as_array().size() != 2 ||
                !owner.as_array()[0].is_string() || owner.as_array()[0].as_string().empty() ||
                !owner.as_array()[1].is_string() || owner.as_array()[1].as_string().empty() ||
                !indexed.owners.emplace(owner.as_array()[0].as_string(), owner.as_array()[1].as_string()).second)
            {
                throw std::runtime_error("usage_cache_invalid");
            }
        }
        for (const auto& message : messages->as_array()) {
            if (context.cancel_requested()) {
                throw std::runtime_error("cancelled");
            }
            if (!message.is_array() || message.as_array().size() != 2 ||
                !message.as_array()[0].is_string() || message.as_array()[0].as_string().empty() ||
                !message.as_array()[1].is_string() || message.as_array()[1].as_string().empty())
            {
                throw std::runtime_error("usage_cache_invalid");
            }
            indexed.messages.emplace_back(message.as_array()[0].as_string(), message.as_array()[1].as_string());
        }
    }
    if (stream.failed() || !header) {
        throw std::runtime_error("usage_cache_unavailable");
    }
    return result;
}

void write_index(const fs::path& root, const Usage_index& index, addon::Invocation_context& context)
{
    std::ofstream stream(root / k_index_file, std::ios::binary | std::ios::trunc);
    Json header = Json::object();
    header.set("format", Json(k_index_format));
    header.set("seed_sha256_hex", Json(index.seed_receipt));
    stream << addon::serialize_json(header) << '\n';
    for (const auto& [cwd, path] : index.repositories) {
        if (context.cancel_requested()) {
            throw std::runtime_error("cancelled");
        }
        Json entry = Json::object();
        entry.set("cwd", Json(cwd));
        entry.set("repository", path ? Json(*path) : Json());
        stream << addon::serialize_json(entry) << '\n';
    }
    for (const auto& [source, indexed] : index.sources) {
        auto next_message = indexed.messages.begin();
        auto next_directory = indexed.directories.begin();
        auto next_owner = indexed.owners.begin();
        bool first = true;
        do {
            if (context.cancel_requested()) {
                throw std::runtime_error("cancelled");
            }
            Json value = Json::object();
            value.set("path", Json(source));
            if (first && indexed.fingerprint) {
                value.set("size", Json(indexed.fingerprint->size));
                value.set("modified", Json(indexed.fingerprint->modified));
            }
            std::size_t bytes = 0;
            Json messages = Json::array();
            for (; next_message != indexed.messages.end() && bytes < k_cache_line_bytes; ++next_message) {
                if (context.cancel_requested()) {
                    throw std::runtime_error("cancelled");
                }
                Json message = Json::array();
                message.push_back(Json(next_message->first));
                message.push_back(Json(next_message->second));
                bytes += addon::serialize_json(message).size();
                messages.push_back(std::move(message));
            }
            value.set("messages", std::move(messages));
            Json directories = Json::array();
            for (; next_directory != indexed.directories.end() && bytes < k_cache_line_bytes; ++next_directory) {
                bytes += next_directory->size();
                directories.push_back(Json(*next_directory));
            }
            value.set("directories", std::move(directories));
            Json owners = Json::array();
            for (; next_owner != indexed.owners.end() && bytes < k_cache_line_bytes; ++next_owner) {
                Json owner = Json::array();
                owner.push_back(Json(next_owner->first));
                owner.push_back(Json(next_owner->second));
                bytes += addon::serialize_json(owner).size();
                owners.push_back(std::move(owner));
            }
            value.set("owners", std::move(owners));
            stream << addon::serialize_json(value) << '\n';
            first = false;
        }
        while (next_message != indexed.messages.end() || next_directory != indexed.directories.end() ||
            next_owner != indexed.owners.end());
    }
    stream.close();
    if (!stream) {
        throw std::runtime_error("usage_cache_write_failed");
    }
}

void check_index_inventory(const Usage_index& index, const addon::Usage_cache_manifest& manifest)
{
    std::set<std::string> expected{k_index_file};
    for (const auto& [source, indexed] : index.sources) {
        (void)indexed;
        expected.insert(source_piece(source));
    }
    if (expected.size() != manifest.files.size()) {
        throw std::runtime_error("usage_cache_invalid");
    }
    for (const auto& file : manifest.files) {
        if (!expected.contains(file.path)) {
            throw std::runtime_error("usage_cache_invalid");
        }
    }
}

void read_source_piece(
    const fs::path& root, const std::string& source, const Usage_index& index,
    File_cache& cache, addon::Invocation_context& context)
{
    File_cache piece;
    const auto receipt = read_cache(root / source_piece(source), piece, context);
    const auto found = piece.find(source);
    if (piece.size() != 1 || found == piece.end() || receipt != index.seed_receipt) {
        throw std::runtime_error("usage_cache_invalid");
    }
    const auto observed = index_source(found->second);
    const auto& expected = index.sources.at(source);
    if (observed.fingerprint != expected.fingerprint || observed.messages != expected.messages ||
        observed.directories != expected.directories || observed.owners != expected.owners)
    {
        throw std::runtime_error("usage_cache_invalid");
    }
    cache.emplace(source, std::move(found->second));
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

std::vector<Usage_session> aggregate_usage(
    const File_cache& cache, addon::Invocation_context& context,
    const std::set<std::string>* selected_ids = nullptr, const Repository_map* known_repositories = nullptr)
{
    // Messages share a few working directories; probe each one once per scan.
    Repository_map repositories = known_repositories ? *known_repositories : Repository_map{};
    const auto repository_of = [&repositories](const std::string& cwd) -> const std::optional<std::string>& {
        auto found = repositories.find(cwd);
        if (found == repositories.end()) {
            found = repositories.emplace(cwd, repository(cwd)).first;
        }
        return found->second;
    };
    // The retained messages stay in the cache; deduplication keys on their IDs
    // and refers to each ID's first copy in the order the cache holds them.
    std::unordered_map<std::string_view, merged_message_t> merged;
    std::map<std::string, const Message*> accepted_owners;
    std::vector<const merged_message_t*> order;
    for (const auto& [path, file] : cache) {
        (void)path;
        for (const auto& record : file.messages) {
            if (context.cancel_requested()) throw std::runtime_error("cancelled");
            if (selected_ids && !selected_ids->contains(record.id)) {
                continue;
            }
            if (record.owner_session && !accepted_owners.emplace(record.id, &record).second) {
                throw std::runtime_error("usage_cache_invalid");
            }
            const auto [previous, inserted] = merged.try_emplace(
                record.id, merged_message_t{&record, record.categories, false, false,
                    record.instant, false, record.temporal_unsupported, record.temporal_retained});
            if (inserted) {
                order.push_back(&previous->second);
                continue;
            }
            auto& existing = previous->second;
            if (existing.first->total != record.total) throw std::runtime_error("conflicting_message_usage");
            if (!existing.temporal_conflicted) {
                if (existing.instant && record.instant && existing.instant != record.instant) {
                    existing.instant.reset();
                    existing.temporal_conflicted = true;
                }
                else
                if (!existing.instant && record.instant) {
                    existing.instant = record.instant;
                }
            }
            existing.temporal_unsupported |= record.temporal_unsupported;
            existing.temporal_retained |= record.temporal_retained;
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
    for (const auto& [id, owner] : accepted_owners) {
        merged.at(id).first = owner;
    }
    std::map<std::string, Usage_session> sessions;
    const std::optional<std::string> unattributed;
    for (const auto* entry : order) {
        if (context.cancel_requested()) throw std::runtime_error("cancelled");
        const auto& message = *entry->first;
        const auto& owner_session = message.owner_session ? *message.owner_session : message.session;
        auto& session = sessions[owner_session];
        session.record.record_id = owner_session;
        session.record.session_key = owner_session;
        session.record.source_path = message.source;
        const auto previous_model = session.record.model.value_or("");
        session.record.model = previous_model.empty() || previous_model == message.model ? message.model : "multiple";
        session.record.completed_date = std::max(session.record.completed_date, message.date);
        add(session.record.total_tokens, message.total);
        session.temporal_retained |= entry->temporal_retained;
        if (!session.temporal_capacity && entry->instant) {
            if (message.total && !session.temporal_tokens.contains(*entry->instant) &&
                (session.temporal_tokens.size() == addon::k_max_usage_temporal_portions ||
                    entry->instant->size() > addon::k_max_usage_temporal_bytes - session.temporal_position_bytes))
            {
                session.temporal_tokens.clear();
                session.temporal_capacity = true;
            }
            else
            if (message.total) {
                auto [position, inserted] = session.temporal_tokens.try_emplace(*entry->instant, 0);
                if (inserted) session.temporal_position_bytes += entry->instant->size();
                add(position->second, message.total);
            }
        }
        else
        if (!entry->instant) {
            add(session.temporal_unavailable_tokens, message.total);
            session.temporal_unsupported |= entry->temporal_unsupported || entry->temporal_conflicted;
            session.temporal_missing |= !entry->temporal_unsupported && !entry->temporal_conflicted;
        }
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
    return result;
}

std::vector<fs::path> transcript_files(const fs::path& profile, addon::Invocation_context& context)
{
    const auto projects = profile / "projects";
    const auto io_projects = native_io_path(projects);
    std::error_code filesystem_error;
    if (!fs::is_directory(native_io_path(profile), filesystem_error) || filesystem_error) {
        throw std::runtime_error("usage_projects_unavailable");
    }
    const bool projects_present = fs::exists(io_projects, filesystem_error);
    if (filesystem_error) {
        throw std::runtime_error("usage_projects_unavailable");
    }
    if (projects_present && !fs::is_directory(io_projects, filesystem_error)) {
        throw std::runtime_error("usage_projects_unavailable");
    }
    if (filesystem_error) {
        throw std::runtime_error("usage_projects_unavailable");
    }
    std::vector<fs::path> files;
    if (projects_present) for (const auto& entry : fs::recursive_directory_iterator(io_projects)) {
        if (context.cancel_requested()) throw std::runtime_error("cancelled");
        if (entry.is_regular_file() && entry.path().extension() == ".jsonl") {
            files.push_back(projects / entry.path().lexically_relative(io_projects));
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

} // namespace

std::vector<Usage_session> scan_usage(const fs::path& profile, addon::Invocation_context& context, const Usage_cache_input& cache_input)
{
    std::lock_guard cache_lock(s_cache_mutex);
    context.report_progress(0, std::nullopt);
    File_cache cache;
    std::string receipt;
    if (cache_input.selected_directory) {
        const auto index = read_index(*cache_input.selected_directory, context);
        receipt = index.seed_receipt;
        for (const auto& [source, indexed] : index.sources) {
            (void)indexed;
            read_source_piece(*cache_input.selected_directory, source, index, cache, context);
        }
    }
    else
    if (cache_input.selected_file && fs::is_regular_file(*cache_input.selected_file)) {
        receipt = read_cache(*cache_input.selected_file, cache, context);
    }
    const auto files = transcript_files(profile, context);
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
    auto result = aggregate_usage(cache, context);
    if (cache_input.storage_file) write_cache(*cache_input.storage_file, cache, context, receipt);
    return result;
}

namespace {

void complete_records(
    Snapshot& snapshot, const std::string& snapshot_id,
    const std::optional<addon::Usage_cache_reference>& accepted_cache)
{
    for (auto& session : snapshot.sessions) {
        addon::Usage_temporal temporal;
        temporal.parent = {session.record.record_id, session.record.total_tokens,
            snapshot_id, snapshot.scope.source_account_key};
        temporal.unavailable_tokens = session.temporal_unavailable_tokens;
        for (const auto& [instant, tokens] : session.temporal_tokens) {
            temporal.portions.push_back({"instant", instant, {}, {}, tokens});
        }
        if (session.temporal_capacity) {
            temporal.unavailable_tokens = session.record.total_tokens;
            temporal.coverage_reason = "capacity";
        }
        else
        if (session.temporal_missing || session.temporal_unsupported) {
            temporal.coverage_reason = temporal.portions.empty() &&
                session.temporal_unsupported && !session.temporal_missing ? "unsupported" : "missing-evidence";
        }
        if (session.temporal_retained && accepted_cache) {
            const auto& accepted = *accepted_cache;
            temporal.retained_from = addon::Usage_temporal_retained_from{
                accepted.snapshot_id, accepted.manifest_sha256_hex};
        }
        if (!addon::decode_usage_temporal(addon::encode_usage_temporal(temporal), nullptr)) {
            temporal.portions.clear();
            temporal.unavailable_tokens = session.record.total_tokens;
            temporal.coverage_reason = "capacity";
            if (!addon::decode_usage_temporal(addon::encode_usage_temporal(temporal), nullptr)) continue;
        }
        session.record.temporal = std::move(temporal);
    }
    for (auto& session : snapshot.sessions) {
        session.record.aggregate_evidence = "partial";
    }
}

addon::Usage_scan_page snapshot_page(
    const addon::Usage_scan_request& input, const std::string& profile_key,
    const std::string& snapshot_id, std::size_t begin,
    std::size_t reserved_payload_bytes = 0)
{
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
        page.account_context = input.context.account_context;
        page.profile_storage = input.context.profile_storage;
        auto end = begin;
        std::size_t encoded_records_bytes = 0;
        for (; end < snapshot.sessions.size(); ++end) {
            page.complete = end + 1 == snapshot.sessions.size();
            if (page.complete) page.next_cursor.reset();
            else page.next_cursor = std::to_string(end + 1);
            const auto& record = snapshot.sessions[end].record;
            if (addon::append_usage_page_record(page, record, encoded_records_bytes, 3, reserved_payload_bytes)) continue;
            if (!page.records.empty()) break;
            auto accounting = record;
            if (accounting.temporal) {
                accounting.temporal->portions.clear();
                accounting.temporal->unavailable_tokens = accounting.total_tokens;
                accounting.temporal->coverage_reason = "capacity";
                if (addon::append_usage_page_record(page, accounting, encoded_records_bytes, 3, reserved_payload_bytes)) continue;
                accounting.temporal.reset();
            }
            if (addon::append_usage_page_record(page, accounting, encoded_records_bytes, 3, reserved_payload_bytes)) continue;
            throw std::runtime_error("usage_record_transport_limit_exceeded");
        }
        page.complete = end == snapshot.sessions.size();
        if (!page.complete) page.next_cursor = std::to_string(end);
        else s_snapshots.erase(found);
    }
    return page;
}

void validate_cache_storage(const addon::Usage_scan_request& input, addon::Contract_failure& failure)
{
    const auto& selection = *input.cache_selection;
    const auto& root = input.context.profile_storage->root;
    if (!addon::validate_usage_cache_root(selection.candidate_root, root, &failure)) {
        throw std::runtime_error(failure.reason_code);
    }
}

struct Message_owner
{
    std::string source;
    std::string session;
};

using Message_owners = std::map<std::string, Message_owner>;

template<class Insert_owner>
void check_index_owners(const Usage_index& index, Insert_owner&& insert_owner)
{
    for (const auto& [source, indexed] : index.sources) {
        std::set<std::string_view> ids;
        for (const auto& [id, session] : indexed.messages) {
            (void)session;
            ids.insert(id);
        }
        for (const auto& [id, session] : indexed.owners) {
            if (!ids.contains(id) || !insert_owner(source, id, session)) {
                throw std::runtime_error("usage_cache_invalid");
            }
        }
    }
}

void validate_index_owners(const Usage_index& index)
{
    std::set<std::string_view> owned;
    check_index_owners(index, [&](const auto&, const auto& id, const auto&) {
        return owned.insert(id).second;
    });
}

Message_owners assign_index_owners(Usage_index& index)
{
    Message_owners owners;
    check_index_owners(index, [&](const auto& source, const auto& id, const auto& session) {
        return owners.emplace(id, Message_owner{source, session}).second;
    });
    for (auto& [source, indexed] : index.sources) {
        for (const auto& [id, session] : indexed.messages) {
            if (owners.try_emplace(id, Message_owner{source, session}).second) {
                indexed.owners.emplace(id, session);
            }
        }
    }
    return owners;
}

void apply_message_owners(File_cache& cache, const Message_owners& owners)
{
    for (auto& [source, cached] : cache) {
        std::set<std::string> owned;
        for (auto& message : cached.messages) {
            const auto& owner = owners.at(message.id);
            message.owner_session = owner.source == source && owned.insert(message.id).second ?
                std::optional<std::string>(owner.session) : std::nullopt;
        }
    }
}

std::map<std::string, std::string> message_owners(const Usage_index& index)
{
    std::map<std::string, std::string> owners;
    for (const auto& [source, indexed] : index.sources) {
        (void)source;
        for (const auto& [id, session] : indexed.owners) {
            if (!owners.emplace(id, session).second) {
                throw std::runtime_error("usage_cache_invalid");
            }
        }
    }
    return owners;
}

Json comparison_record(addon::Usage_record record)
{
    if (record.temporal) {
        record.temporal->retained_from.reset();
    }
    return addon::encode_usage_record(record, 3);
}

void handle_incremental_usage(addon::Invocation_context& context, const char*& stage)
{
    if (context.request().operation != "scan") {
        context.reject_unknown_operation();
        return;
    }
    addon::Contract_failure failure;
    const auto input = addon::decode_usage_incremental_request(context.request().payload, &failure);
    if (!input) {
        context.fail(addon::Result_status::FAILED, failure.reason_code);
        return;
    }
    const auto& baseline = *input->cache_selection->accepted;
    const auto& storage = input->context.profile_storage->root;
    const auto& candidate = input->cache_selection->candidate_root;
    const auto cancelled = [&context] { return context.cancel_requested(); };
    addon::Usage_incremental_result result;
    result.baseline = baseline;
    result.account_context = input->context.account_context;
    result.profile_storage = input->context.profile_storage;
    stage = "selecting the Claude profile";
    const auto profile = profile_for_context(input->context);
    auto identity = *input;
    identity.snapshot_id.reset();
    identity.cursor.reset();
    const auto profile_key = "incremental:" + addon::to_hex(addon::Sha256::digest(
        addon::serialize_json_canonical(addon::encode_usage_incremental_request(identity))));
    stage = "checking usage cache storage";
    validate_cache_storage(*input, failure);
    std::string snapshot_id;
    std::size_t begin = 0;
    if (input->snapshot_id) {
        snapshot_id = *input->snapshot_id;
        const auto& cursor = *input->cursor;
        const auto parsed = std::from_chars(cursor.data(), cursor.data() + cursor.size(), begin);
        if (parsed.ec != std::errc{} || parsed.ptr != cursor.data() + cursor.size() ||
            std::to_string(begin) != cursor || begin == 0)
        {
            throw std::runtime_error("usage_cursor_invalid");
        }
    }
    else {
        std::lock_guard cache_lock(s_cache_mutex);
        context.report_progress(0, std::nullopt);
        if (!addon::validate_usage_cache_allocation(candidate, storage, &failure)) {
            throw std::runtime_error(failure.reason_code);
        }
        const auto manifest = addon::validate_usage_cache_manifest(
            baseline, storage, input->context.account_context, cancelled, &failure);
        if (!manifest) {
            throw std::runtime_error(failure.reason_code);
        }
        const auto selected_root = addon::usage_cache_io_path(addon::path_from_utf8(baseline.root));
        const auto candidate_root = addon::usage_cache_io_path(addon::path_from_utf8(candidate));
        Usage_index index;
        File_cache retained;
        const bool adoption = manifest->format != k_index_format;
        stage = "verifying the saved usage cache";
        if (adoption) {
            if (manifest->format != "claude-message-cache@2" ||
                manifest->files.size() != 1 || manifest->files.front().path != "messages.jsonl")
            {
                throw std::runtime_error("usage_cache_format_unsupported");
            }
            if (!addon::validate_usage_cache_file(baseline, *manifest, "messages.jsonl", cancelled, &failure)) {
                throw std::runtime_error(failure.reason_code);
            }
            index.seed_receipt = read_cache(selected_root / "messages.jsonl", retained, context);
            for (const auto& [source, cached] : retained) {
                index.sources.emplace(source, index_source(cached));
                for (const auto& message : cached.messages) {
                    if (!index.repositories.contains(message.cwd)) {
                        index.repositories.emplace(message.cwd, repository(message.cwd));
                    }
                }
            }
            apply_message_owners(retained, assign_index_owners(index));
        }
        else {
            if (!addon::validate_usage_cache_file(baseline, *manifest, k_index_file, cancelled, &failure)) {
                throw std::runtime_error(failure.reason_code);
            }
            index = read_index(selected_root, context);
            check_index_inventory(index, *manifest);
        }
        const auto load_source = [&](const std::string& source) {
            if (retained.contains(source)) {
                return;
            }
            if (!addon::validate_usage_cache_file(baseline, *manifest, source_piece(source), cancelled, &failure)) {
                throw std::runtime_error(failure.reason_code);
            }
            read_source_piece(selected_root, source, index, retained, context);
        };
        stage = "reading usage history and saving its candidate cache";
        File_cache updated;
        std::set<std::string> changed_ids;
        std::optional<Usage_index> candidate_index;
        const auto files = transcript_files(profile, context);
        std::uint64_t processed = 0;
        for (const auto& file : files) {
            if (cancelled()) {
                throw std::runtime_error("cancelled");
            }
            const auto source = cache_path(file);
            const auto previous = index.sources.find(source);
            if (previous == index.sources.end() || previous->second.fingerprint != fingerprint(file)) {
                file_fingerprint_t observed;
                if (auto parsed = parse_file(file, context, observed)) {
                    Cached_file fresh;
                    fresh.fingerprint = observed;
                    if (previous != index.sources.end()) {
                        load_source(source);
                        fresh.messages = reconcile_messages(retained.at(source).messages, std::move(*parsed), context);
                        for (const auto& [id, session] : previous->second.messages) {
                            (void)session;
                            changed_ids.insert(id);
                        }
                    }
                    else {
                        fresh.messages = std::move(*parsed);
                    }
                    for (const auto& message : fresh.messages) {
                        changed_ids.insert(message.id);
                    }
                    if (!candidate_index) {
                        candidate_index.emplace(index);
                    }
                    candidate_index->sources[source] = index_source(fresh);
                    updated.emplace(source, std::move(fresh));
                }
            }
            context.report_progress(++processed, files.size());
        }
        Repository_map current_repositories;
        const auto& observed_index = candidate_index ? *candidate_index : index;
        for (const auto& [source, indexed] : observed_index.sources) {
            (void)source;
            for (const auto& cwd : indexed.directories) {
                if (cancelled()) {
                    throw std::runtime_error("cancelled");
                }
                if (!current_repositories.contains(cwd)) {
                    current_repositories.emplace(cwd, repository(cwd));
                }
                const auto previous = index.repositories.find(cwd);
                if (previous != index.repositories.end() && previous->second != current_repositories.at(cwd)) {
                    for (const auto& [id, session] : indexed.messages) {
                        (void)session;
                        changed_ids.insert(id);
                    }
                }
            }
        }
        if (!adoption && updated.empty() && current_repositories == index.repositories) {
            // Saved owners still need membership and uniqueness checks on an unchanged scan.
            validate_index_owners(index);
            if (cancelled()) {
                throw std::runtime_error("cancelled");
            }
            context.succeed(addon::encode_usage_incremental_result(result));
            return;
        }
        if (!candidate_index) {
            candidate_index.emplace(index);
        }
        auto& next_index = *candidate_index;
        next_index.repositories = std::move(current_repositories);
        const auto owners = assign_index_owners(next_index);
        apply_message_owners(updated, owners);
        Snapshot snapshot;
        snapshot.profile = profile_key;
        snapshot.scope.observed_at_ms = now_ms();
        snapshot.scope.fresh_for_ms = 60000;
        if (input->context.account_context) {
            snapshot.scope.source_account_key = input->context.account_context->adapter_account_key;
        }
        snapshot_id = addon::random_nonce_hex();
        if (adoption) {
            for (const auto& [source, fresh] : updated) {
                retained[source] = fresh;
            }
            // The monolithic format retained cwd, but not the resolved repository
            // allocation. Publish its saved observations once during conversion.
            snapshot.sessions = aggregate_usage(retained, context, nullptr, &next_index.repositories);
            complete_records(snapshot, snapshot_id, baseline);
        }
        else
        if (!changed_ids.empty()) {
            const auto previous_owners = message_owners(index);
            const auto next_owners = message_owners(next_index);
            std::set<std::string> affected_sessions;
            for (const auto& id : changed_ids) {
                for (const auto* owners : {&previous_owners, &next_owners}) {
                    const auto found = owners->find(id);
                    if (found != owners->end()) {
                        affected_sessions.insert(found->second);
                    }
                }
            }
            std::set<std::string> selected_ids;
            for (const auto* owners : {&previous_owners, &next_owners}) {
                for (const auto& [id, session] : *owners) {
                    if (affected_sessions.contains(session)) {
                        selected_ids.insert(id);
                    }
                }
            }
            // A changed message can affect the first copy in another transcript;
            // all copies of every affected session's messages participate.
            for (const auto& [source, indexed] : index.sources) {
                if (std::any_of(indexed.messages.begin(), indexed.messages.end(),
                    [&selected_ids](const auto& message) { return selected_ids.contains(message.first); }))
                {
                    load_source(source);
                }
            }
            Snapshot previous;
            previous.scope = snapshot.scope;
            previous.sessions = aggregate_usage(retained, context, &selected_ids, &index.repositories);
            complete_records(previous, snapshot_id, baseline);
            for (const auto& [source, fresh] : updated) {
                retained[source] = fresh;
            }
            snapshot.sessions = aggregate_usage(retained, context, &selected_ids, &next_index.repositories);
            complete_records(snapshot, snapshot_id, baseline);
            std::map<std::string, Json> previous_records;
            for (const auto& session : previous.sessions) {
                previous_records.emplace(session.record.record_id, comparison_record(session.record));
            }
            std::erase_if(snapshot.sessions, [&previous_records](const Usage_session& session) {
                const auto found = previous_records.find(session.record.record_id);
                return found != previous_records.end() && found->second == comparison_record(session.record);
            });
        }
        for (const auto& [source, indexed] : next_index.sources) {
            (void)indexed;
            if (adoption || updated.contains(source)) {
                File_cache piece;
                const auto fresh = updated.find(source);
                piece.emplace(source, fresh == updated.end() ? retained.at(source) : fresh->second);
                write_cache(candidate_root / source_piece(source), piece, context, index.seed_receipt);
            }
            else
            if (!addon::inherit_usage_cache_file(
                baseline, *manifest, source_piece(source), candidate, storage, cancelled, &failure))
            {
                throw std::runtime_error(failure.reason_code);
            }
        }
        write_index(candidate_root, next_index, context);
        stage = "sealing the candidate usage cache";
        snapshot.cache_candidate = addon::seal_usage_incremental_cache_candidate(
            candidate, storage, input->context.account_context, snapshot_id,
            std::string(k_index_format), baseline, cancelled, &failure);
        if (!snapshot.cache_candidate) {
            throw std::runtime_error(failure.reason_code);
        }
        snapshot.touched_ms = now_ms();
        std::lock_guard lock(s_snapshot_mutex);
        std::erase_if(s_snapshots, [](const auto& entry) { return now_ms() - entry.second.touched_ms > 600000; });
        if (s_snapshots.size() >= 8) {
            throw std::runtime_error("usage_snapshot_capacity");
        }
        s_snapshots.emplace(snapshot_id, std::move(snapshot));
    }
    stage = "returning the usage snapshot";
    result.changes = addon::Usage_scan_page{};
    const auto reserved_payload_bytes = addon::serialize_json(addon::encode_usage_incremental_result(result)).size() -
        addon::serialize_json(addon::encode_usage_scan_result(*result.changes, 3)).size();
    result.changes = snapshot_page(*input, profile_key, snapshot_id, begin, reserved_payload_bytes);
    context.succeed(addon::encode_usage_incremental_result(result));
}

void handle_usage_scan(addon::Invocation_context& context, const char*& stage)
{
    if (context.request().operation != "scan") {
        context.reject_unknown_operation();
        return;
    }
    addon::Contract_failure failure;
    const auto input = addon::decode_usage_scan_request(context.request().payload, &failure, 3);
    if (!input) {
        context.fail(addon::Result_status::FAILED, failure.reason_code);
        return;
    }
    stage = "selecting the Claude profile";
    const auto profile = profile_for_context(input->context);
    stage = "checking usage cache storage";
    auto identity = *input;
    identity.snapshot_id.reset();
    identity.cursor.reset();
    const auto profile_key = addon::to_hex(addon::Sha256::digest(
        addon::serialize_json_canonical(addon::encode_usage_scan_request(identity, 3))));
    validate_cache_storage(*input, failure);
    std::string snapshot_id;
    std::size_t begin = 0;
    if (!input->snapshot_id) {
        Snapshot snapshot;
        snapshot.profile = profile_key;
        if (!input->context.profile_storage) throw std::runtime_error("profile_storage_unavailable");
        Usage_cache_input cache;
        cache.incremental  = input->mode == "incremental";
        const auto& selection = *input->cache_selection;
        const auto& root = input->context.profile_storage->root;
        if (!addon::validate_usage_cache_allocation(selection.candidate_root, root, &failure)) {
            throw std::runtime_error(failure.reason_code);
        }
        cache.storage_file = addon::usage_cache_io_path(addon::path_from_utf8(selection.candidate_root)) / "messages.jsonl";
        if (selection.accepted) {
            stage = "verifying the saved usage cache";
            const auto manifest = addon::validate_usage_cache_candidate(
                *selection.accepted, root, input->context.account_context,
                [&context] { return context.cancel_requested(); }, &failure);
            if (!manifest) throw std::runtime_error(failure.reason_code);
            if (manifest->format == k_index_format) {
                const auto directory = addon::usage_cache_io_path(addon::path_from_utf8(selection.accepted->root));
                check_index_inventory(read_index(directory, context), *manifest);
                cache.selected_directory = directory;
            }
            else
            if (manifest->format != "claude-message-cache@2" ||
                manifest->files.size() != 1 ||
                manifest->files.front().path != "messages.jsonl")
            {
                throw std::runtime_error("usage_cache_format_unsupported");
            }
            else {
                cache.selected_file = addon::usage_cache_io_path(addon::path_from_utf8(selection.accepted->root)) / "messages.jsonl";
            }
        }
        stage = "reading usage history and saving its candidate cache";
        snapshot.sessions = scan_usage(profile, context, cache);
        snapshot.scope.observed_at_ms = now_ms();
        snapshot.scope.fresh_for_ms = 60000;
        if (input->context.account_context) snapshot.scope.source_account_key = input->context.account_context->adapter_account_key;
        snapshot.touched_ms = now_ms();
        snapshot_id = addon::random_nonce_hex();
        complete_records(snapshot, snapshot_id, input->cache_selection->accepted);
        stage = "sealing the candidate usage cache";
        snapshot.cache_candidate = addon::seal_usage_cache_candidate(
            input->cache_selection->candidate_root, input->context.profile_storage->root,
            input->context.account_context, snapshot_id, "claude-message-cache@2",
            [&context] { return context.cancel_requested(); }, &failure);
        if (!snapshot.cache_candidate) throw std::runtime_error(failure.reason_code);
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
    stage = "returning the usage snapshot";
    const auto page = snapshot_page(*input, profile_key, snapshot_id, begin);
    context.succeed(addon::encode_usage_scan_result(page, 3));
}

void fail_usage(addon::Invocation_context& context, const char* stage,
    const char* reason, const std::string& explanation)
{
    if (context.cancel_requested()) {
        context.fail(addon::Result_status::CANCELLED, "cancelled");
        return;
    }
    context.fail(addon::Result_status::FAILED, reason,
        "Claude could not finish " + std::string(stage) + ". " + explanation);
}

} // namespace

void handle_usage(addon::Invocation_context& context)
{
    const char* stage = "preparing the usage scan";
    try {
        if (context.request().capability.capability_id == "usage-incremental") {
            handle_incremental_usage(context, stage);
        }
        else {
            handle_usage_scan(context, stage);
        }
    }
    catch (const fs::filesystem_error& error) {
        fail_usage(context, stage, "claude_usage_filesystem_failed",
            "A filesystem operation failed (system error " + std::to_string(error.code().value()) + ").");
    }
    catch (const std::bad_alloc&) {
        fail_usage(context, stage, "claude_usage_memory_unavailable",
            "The provider could not allocate the memory needed to complete the scan.");
    }
    catch (const std::exception& error) {
        // Only exact provider-owned reasons may leave this boundary. Other
        // exceptions can contain transcript text, credentials or native paths.
        constexpr std::pair<const char*, const char*> reasons[] = {
            {"account_unavailable", "The selected account has no available profile."},
            {"account_profile_mismatch", "The selected profile does not match its account."},
            {"launch_environment_unavailable", "The launcher environment is unavailable."},
            {"profile_normalization_failed", "The selected profile could not be resolved."},
            {"usage_projects_unavailable", "The profile's transcript directory is unavailable."},
            {"usage_json_invalid", "A usage record is not valid JSON."},
            {"usage_record_capacity", "A usage record exceeds the supported record size."},
            {"usage_timestamp_invalid", "A usage record has an invalid timestamp."},
            {"usage_counter_invalid", "A usage record has an invalid token count."},
            {"usage_overflow", "The token counts exceed the range that can be represented exactly."},
            {"usage_file_unreadable", "A transcript could not be read."},
            {"usage_cache_path_invalid", "A saved usage cache path is invalid."},
            {"usage_cache_invalid", "The saved usage cache contains invalid records."},
            {"usage_cache_unavailable", "The usage cache is unavailable or could not be verified."},
            {"usage_cache_incomplete", "The saved usage cache is incomplete."},
            {"usage_cache_format_unsupported", "The saved usage cache format is unsupported."},
            {"usage_cache_write_failed", "The candidate usage cache could not be written."},
            {"usage_cache_namespace_conflict", "The candidate cache conflicts with another cache location."},
            {"conflicting_message_usage", "Copies of the same message disagree about its token count."},
            {"profile_storage_unavailable", "The provider's usage cache storage is unavailable."},
            {"usage_snapshot_capacity", "The provider is retaining too many unfinished usage snapshots."},
            {"usage_snapshot_unavailable", "The requested usage snapshot is no longer available."},
            {"usage_cursor_invalid", "The requested usage page is invalid."},
            {"usage_record_transport_limit_exceeded", "An accounting record exceeds the supported transport size."},
        };
        for (const auto& [reason, explanation] : reasons) {
            if (std::string_view(error.what()) == reason) {
                fail_usage(context, stage, reason, explanation);
                return;
            }
        }
        fail_usage(context, stage, "claude_usage_failed", "An unexpected provider error occurred.");
    }
}

} // namespace claude
