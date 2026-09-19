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
#include <utility>

namespace claude {
namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
using Json = addon::Json_value;

constexpr std::uint64_t k_max_exact = 9007199254740991;
constexpr std::uint64_t k_max_history_bytes = 512 * 1024 * 1024;
constexpr std::size_t k_max_messages = 1000000;

Json usage_object(std::string_view bytes)
{
    addon::Json_parse_limits limits;
    limits.max_bytes = k_max_history_bytes;
    limits.max_string_bytes = k_max_history_bytes;
    addon::Json_parse_error failure;
    auto value = addon::parse_json(bytes, limits, &failure);
    if (!value || !value->is_object()) throw std::runtime_error("usage_json_invalid");
    return std::move(*value);
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
    bool breakdown_conflicted = false;
    bool repository_conflicted = false;
};

struct Snapshot
{
    addon::Observation_scope scope;
    std::string profile;
    std::vector<Usage_session> sessions;
    std::uint64_t touched_ms = 0;
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

std::vector<Message> parse_file(const fs::path& path, addon::Invocation_context& context, std::uint64_t& bytes_read)
{
    const auto size = fs::file_size(path);
    const auto modified = fs::last_write_time(path);
    if (size > k_max_history_bytes - bytes_read) throw std::runtime_error("usage_history_capacity");
    bytes_read += size;
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("usage_file_unreadable");
    std::vector<Message> result;
    std::string line;
    while (std::getline(file, line)) {
        if (context.cancel_requested()) throw std::runtime_error("cancelled");
        if (file.eof()) break; // An unterminated record is still being written.
        const auto first = line.find_first_not_of('\0');
        const auto event = usage_object(first == std::string::npos ? std::string_view{} : std::string_view(line).substr(first));
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
        record.source = path_string(fs::absolute(path).lexically_normal());
        record.cwd    = text(event, "cwd");
        record.model  = text(*message, "model");
        record.categories = {count(values, "input_tokens"), count(values, "output_tokens"),
            count(values, "cache_read_input_tokens"), count(values, "cache_creation_input_tokens")};
        for (const auto& category : record.categories) {
            if (category) add(record.total, *category);
        }
        if (result.size() >= k_max_messages) throw std::runtime_error("usage_history_capacity");
        result.push_back(std::move(record));
    }
    if (file.bad() || fs::file_size(path) != size || fs::last_write_time(path) != modified) {
        throw std::runtime_error("usage_file_changed");
    }
    return result;
}


using File_cache = std::map<std::string, std::vector<Message>>;

std::vector<Message> reconcile_messages(
    const std::vector<Message>& retained, std::vector<Message> observed)
{
    std::set<std::string> observed_ids;
    for (const auto& message : observed) {
        observed_ids.insert(message.id);
    }
    // A completed observation can correct its own message. Missing or unfinished
    // messages provide no evidence that previously completed usage disappeared.
    for (const auto& message : retained) {
        if (!observed_ids.contains(message.id)) {
            if (observed.size() >= k_max_messages) {
                throw std::runtime_error("usage_history_capacity");
            }
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

std::string read_cache(const fs::path& path, bool native, File_cache& cache, addon::Invocation_context& context, std::string_view expected_hash = {})
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("usage_cache_unavailable");
    std::uint64_t bytes = 0;
    addon::Sha256 digest;
    std::string receipt;
    bool header_seen = native;
    std::size_t messages = 0;
    std::string line;
    while (std::getline(stream, line)) {
        if (context.cancel_requested()) throw std::runtime_error("cancelled");
        if (stream.eof()) throw std::runtime_error("usage_cache_incomplete");
        if (line.size() > k_max_history_bytes - bytes) throw std::runtime_error("usage_history_capacity");
        bytes += line.size();
        digest.update(line);
        digest.update("\n");
        const auto wrapper = usage_object(line);
        if (!header_seen) {
            if (text(wrapper, "format") != "claude-message-cache-v1") throw std::runtime_error("usage_cache_invalid");
            receipt = text(wrapper, "seed_sha256_hex");
            header_seen = true;
            continue;
        }
        const auto raw_source = required_text(wrapper, "path");
        if (!addon::path_from_utf8(raw_source).is_absolute()) throw std::runtime_error("usage_cache_invalid");
        const auto source = cache_path(addon::path_from_utf8(raw_source));
        const auto* records = wrapper.find("records");
        if (!records || !records->is_array() || cache.contains(source)) throw std::runtime_error("usage_cache_invalid");
        auto& destination = cache[source];
        for (const auto& record : records->as_array()) {
            if (++messages > k_max_messages) throw std::runtime_error("usage_history_capacity");
            destination.push_back(cached_message(record, native));
        }
    }
    if (stream.bad() || !header_seen) throw std::runtime_error("usage_cache_unavailable");
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
        header.set("format", Json("claude-message-cache-v1"));
        header.set("seed_sha256_hex", Json(seed_receipt));
        stream << addon::serialize_json(header) << '\n';
        for (const auto& [source, records] : cache) {
            if (context.cancel_requested()) throw std::runtime_error("cancelled");
            Json wrapper = Json::object();
            wrapper.set("path", Json(source));
            Json entries = Json::array();
            for (const auto& record : records) entries.push_back(cache_record(record));
            wrapper.set("records", std::move(entries));
            stream << addon::serialize_json(wrapper) << '\n';
        }
        stream.close();
        if (!stream) throw std::runtime_error("usage_cache_write_failed");
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::runtime_error("usage_cache_write_failed");
        }
#else
        // The host removes the native seed after success: both its adopted
        // contents and the replacement directory entry must survive power loss.
        sync_cache_path(temporary, 0);
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
    if (cache_input.storage_file && fs::is_regular_file(*cache_input.storage_file)) {
        receipt = read_cache(*cache_input.storage_file, false, cache, context);
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
    if (cache_input.cache_required && !cache_loaded) throw std::runtime_error("usage_required_cache_unavailable");
    std::vector<fs::path> files;
    if (projects_present) for (const auto& entry : fs::recursive_directory_iterator(projects)) {
        if (context.cancel_requested()) throw std::runtime_error("cancelled");
        if (entry.is_regular_file() && entry.path().extension() == ".jsonl") files.push_back(entry.path());
        if (files.size() > 100000) throw std::runtime_error("usage_history_capacity");
    }
    std::sort(files.begin(), files.end());
    std::map<std::string, Message> messages;
    std::vector<std::string> order;
    std::uint64_t bytes_read = 0;
    std::uint64_t processed = 0;
    for (const auto& file : files) {
        auto observed = parse_file(file, context, bytes_read);
        auto& retained = cache[cache_path(file)];
        retained = reconcile_messages(retained, std::move(observed));
        context.report_progress(++processed, files.size());
    }
    for (const auto& [path, records] : cache) {
        (void)path;
        for (const auto& record : records) {
            const auto previous = messages.find(record.id);
            if (previous == messages.end()) {
                if (messages.size() == k_max_messages) throw std::runtime_error("usage_history_capacity");
                order.push_back(record.id);
                messages.emplace(record.id, record);
                continue;
            }
            auto& existing = previous->second;
            if (existing.total != record.total) throw std::runtime_error("conflicting_message_usage");
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
            if (!existing.repository_conflicted && repository(existing.cwd) != repository(record.cwd)) {
                existing.cwd.clear();
                existing.repository_conflicted = true;
            }
        }
    }
    std::map<std::string, Usage_session> sessions;
    for (const auto& id : order) {
        const auto& message = messages.at(id);
        auto& session = sessions[message.session];
        session.record.record_id = message.session;
        session.record.session_key = message.session;
        session.record.source_path = message.source;
        const auto previous_model = session.record.model.value_or("");
        session.record.model = previous_model.empty() || previous_model == message.model ? message.model : "multiple";
        session.record.completed_date = std::max(session.record.completed_date, message.date);
        add(session.record.total_tokens, message.total);
        ++session.message_count;
        for (std::size_t index = 0; index < message.categories.size(); ++index) {
            if (message.categories[index]) {
                add(session.category_values[index], *message.categories[index]);
                ++session.category_coverage[index];
            }
        }
        const auto repo = repository(message.cwd);
        if (repo) add(session.repository_tokens[*repo], message.total);
        else add(session.unattributed_tokens, message.total);
    }
    std::vector<Usage_session> result;
    for (auto& [id, session] : sessions) {
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
    const auto input = addon::decode_usage_scan_request(context.request().payload, &failure, 2);
    if (!input) {
        context.fail(addon::Result_status::FAILED, failure.reason_code);
        return;
    }
    const auto profile = profile_for_context(input->context);
    const auto profile_key = account_key(profile);
    std::string snapshot_id;
    std::size_t begin = 0;
    if (!input->snapshot_id) {
        Snapshot snapshot;
        snapshot.profile = profile_key;
        if (!input->context.profile_storage) throw std::runtime_error("profile_storage_unavailable");
        Usage_cache_input cache;
        cache.storage_file = addon::path_from_utf8(input->context.profile_storage->root) / "claude-message-cache-v1.jsonl";
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
        const auto end = std::min(begin + addon::k_max_usage_page_records, snapshot.sessions.size());
        for (auto index = begin; index < end; ++index) page.records.push_back(snapshot.sessions[index].record);
        page.complete = end == snapshot.sessions.size();
        if (!page.complete) page.next_cursor = std::to_string(end);
        else s_snapshots.erase(found);
    }
    context.succeed(addon::encode_usage_scan_result(page, 2));
}

} // namespace claude
