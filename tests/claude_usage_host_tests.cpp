// The Logonomic host's usage exchanges with this package across an explicit
// import of retained native history, a rollback and a second import, driven
// through the SDK host simulator.
//
// Each step plays the host's side of usage-scan@3 and legacy-state-import@1:
// it allocates the candidate, selects the cache the host accepted last, and
// writes the retained native seed in the host's FILE_CACHE export form. The
// host keeps that seed across the rollback, so the second import offers the
// same seed together with the cache accepted before the rollback. What is
// checked is only what this package answers; the host's own acceptance and
// rollback rules are tested in Logonomic against fixture packages.

#include "claude_accounts.h"

#include <logonomic_cli_addon/environment_bootstrap.h>
#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/legacy_state_import.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/profile_storage.h>
#include <logonomic_cli_addon/sha256.h>
#include <logonomic_cli_addon/usage_scan.h>
#include <logonomic_cli_addon/usage_temporal.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace addon = logonomic::cli_addon;
namespace fs    = std::filesystem;

constexpr const char* k_package_id = "logonomic.cli.claude";

void check(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void write(const fs::path& path, std::string_view bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << bytes;
    output.close();
    check(static_cast<bool>(output), "write fixture file " + addon::path_to_utf8(path));
}

std::string read(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    check(static_cast<bool>(input), "read " + addon::path_to_utf8(path));
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

class Temporary_directory
{
public:
    Temporary_directory()
    :
        m_path(fs::temp_directory_path() / ("claude-usage-host-" + addon::random_nonce_hex()))
    {
        check(fs::create_directory(m_path), "create isolated fixture");
        m_path = fs::canonical(m_path);
    }

    ~Temporary_directory()
    {
        std::error_code error;
        fs::remove_all(m_path, error);
    }

    const fs::path& path() const { return m_path; }

private:
    fs::path m_path;
};

// The host stores source paths in Qt's spelling, with forward slashes.
std::string host_path(const fs::path& path)
{
    const auto bytes = path.generic_u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

fs::path source_path(const fs::path& profile, std::string_view session)
{
    return profile / "projects" / "repo" / fs::path(std::string(session) + ".jsonl");
}

// One completed assistant message of a native Claude transcript.
std::string transcript_message(std::string_view session, std::string_view message_id, std::uint64_t tokens)
{
    return
        "{\"message\":{\"id\":\"" + std::string(message_id) + "\",\"model\":\"claude-test\","
        "\"stop_reason\":\"end_turn\",\"usage\":{\"input_tokens\":" + std::to_string(tokens) +
        ",\"output_tokens\":0}},\"sessionId\":\"" + std::string(session) +
        "\",\"timestamp\":\"2026-07-20T10:00:00Z\",\"type\":\"assistant\"}\n";
}

addon::Json_value breakdown_field(std::optional<std::uint64_t> tokens)
{
    auto field = addon::Json_value::object();
    field.set("support", addon::Json_value(tokens ? "supported" : "unknown"));
    field.set("covered_records", addon::Json_value(tokens ? 1 : 0));
    field.set("total_records", addon::Json_value(1));
    field.set("known_tokens", addon::Json_value(tokens.value_or(0)));
    return field;
}

struct Retained_message
{
    std::string   session;
    std::string   message_id;
    std::uint64_t tokens = 0;
};

// One line of the host's native FILE_CACHE export for a retained transcript,
// after the host converted each record's identity to this package: the
// stored record keeps its native fields, its session_id becomes the hex of
// the converted record ID, and usage-record-identity carries both identities.
std::string retained_file_seed_line(const fs::path& profile, const Retained_message& retained)
{
    const std::string source = host_path(source_path(profile, retained.session));
    auto breakdown = addon::Json_value::object();
    breakdown.set("version", addon::Json_value(2));
    breakdown.set("coverage_unit", addon::Json_value("records"));
    breakdown.set("input_tokens", breakdown_field(retained.tokens));
    breakdown.set("output_tokens", breakdown_field(0));
    breakdown.set("cache_read_input_tokens", breakdown_field(std::nullopt));
    breakdown.set("cache_creation_input_tokens", breakdown_field(std::nullopt));
    auto identity = addon::Json_value::object();
    identity.set("version", addon::Json_value(1));
    identity.set("record_id", addon::Json_value(retained.session));
    identity.set("session_key", addon::Json_value(retained.session));
    identity.set("legacy_record_id", addon::Json_value(retained.session));
    identity.set("legacy_session_key", addon::Json_value(retained.session));
    auto capabilities = addon::Json_value::object();
    capabilities.set("token_breakdown", std::move(breakdown));
    capabilities.set("usage-record-identity", std::move(identity));

    auto record = addon::Json_value::object();
    record.set("record_id", addon::Json_value(retained.message_id));
    record.set("session_id", addon::Json_value(addon::to_hex(
        reinterpret_cast<const std::uint8_t*>(retained.session.data()), retained.session.size())));
    record.set("completed_date", addon::Json_value("2026-07-20"));
    record.set("total_tokens", addon::Json_value(retained.tokens));
    record.set("source_path", addon::Json_value(source));
    record.set("model", addon::Json_value("claude-test"));
    record.set("counter_field", addon::Json_value(
        "usage.input_tokens + usage.output_tokens + usage.cache_read_input_tokens + usage.cache_creation_input_tokens"));
    record.set("counter_semantics", addon::Json_value("per_event_sum"));
    record.set("capabilities", std::move(capabilities));

    auto markers = addon::Json_value::object();
    auto marker = addon::Json_value::object();
    marker.set("version", addon::Json_value(2));
    marker.set("parser_revision", addon::Json_value(2));
    markers.set("token_breakdown_cache", std::move(marker));
    marker = addon::Json_value::object();
    marker.set("version", addon::Json_value(1));
    marker.set("parser_revision", addon::Json_value(1));
    markers.set("repository_usage_cache", std::move(marker));
    auto records = addon::Json_value::array();
    records.push_back(std::move(record));
    auto file = addon::Json_value::object();
    file.set("path", addon::Json_value(source));
    file.set("size", addon::Json_value(0));
    file.set("modified_msecs", addon::Json_value(0));
    file.set("records", std::move(records));
    file.set("capabilities", std::move(markers));
    // The host serializes stored records with sorted members.
    return addon::serialize_json_canonical(file) + '\n';
}

// The host side of one account's usage exchanges with the packaged adapter.
class Packaged_host
{
public:
    Packaged_host(const fs::path& package, const fs::path& fixture, const fs::path& profile)
    :
        m_host(simulator_options(package, fixture), nullptr)
    {
        addon::Contract_failure failure;
        check(m_host.start(&failure), "packaged adapter starts: " + failure.detail);
        const auto live = m_host.live_capabilities();
        const auto is_live = [&live](const addon::Capability_pair& pair) {
            return std::find(live.begin(), live.end(), pair) != live.end();
        };
        check(is_live({"usage-scan", 3}), "the package negotiates host-selected cache acceptance");
        check(is_live({addon::k_usage_temporal_capability_id, addon::k_usage_temporal_version}),
            "the package negotiates optional temporal evidence independently");
        check(is_live({"legacy-state-import", 1}), "the package negotiates native history import");

        const auto native_profile = addon::path_to_utf8(profile);
        m_account_key = claude::account_key(profile);
        std::string error;
        const auto allocated = addon::allocate_profile_storage_root(
            m_host.profile_storage_root(), k_package_id, &error, m_account_key);
        check(allocated.has_value(), "allocate account profile storage: " + error);
        m_storage_root = *allocated;

        m_request.context.launcher        = {"claude", addon::k_runner_direct, {}};
        m_request.context.launcher_argv   = {"claude"};
        m_request.context.profile_storage = addon::Profile_storage_context{1, m_storage_root};
        m_request.context.account_context = addon::Account_context{2, m_account_key, native_profile};
        m_request.context.launch_environment = addon::Launch_environment{
            addon::host_platform_id(), addon::Json_value::object(),
            addon::Environment_bootstrap_evidence{native_profile}};
        m_invocation.profile_storage = m_request.context.profile_storage;

        const auto acknowledge = m_host.invoke({"profile-storage", 1}, "acknowledge",
            addon::Json_value::object(), 5000, m_invocation);
        const auto acknowledged = m_host.wait_outcome(acknowledge, 6000);
        check(acknowledged && acknowledged->status == addon::Host_status::SUCCESS,
            "packaged adapter acknowledges its account profile storage");
    }

    void finish()
    {
        check(m_host.shutdown(1000, nullptr), "packaged adapter exits by itself, without contacting a model");
    }

    const std::string& account_key() const { return m_account_key; }
    fs::path storage_root() const { return addon::path_from_utf8(m_storage_root); }

    // A fresh, host-owned candidate allocation and the accepted cache the
    // host's durable usage state selects. Continuation state and any native
    // seed from an earlier exchange do not carry over.
    void select(std::string_view name, std::optional<addon::Usage_cache_reference> accepted)
    {
        const auto candidate = storage_root() / fs::path(std::string(name));
        check(fs::create_directory(addon::usage_cache_io_path(candidate)), "allocate candidate " + std::string(name));
        m_request.snapshot_id.reset();
        m_request.cursor.reset();
        m_request.native_file_cache.reset();
        m_request.cache_selection = addon::Usage_cache_selection{std::move(accepted), addon::path_to_utf8(candidate)};
    }

    // The retained native seed as the host writes it: beside the account's
    // storage, with its digest and the account it was exported from.
    std::string seed(std::string_view name, std::string_view bytes, bool cache_required)
    {
        const auto path = storage_root() / fs::path(std::string(name));
        write(path, bytes);
        const auto digest = addon::to_hex(addon::Sha256::digest(bytes));
        m_request.native_file_cache = addon::Usage_native_file_cache{
            addon::path_to_utf8(path), digest, m_account_key, cache_required};
        return digest;
    }

    addon::Host_outcome scan_outcome()
    {
        const auto id = m_host.invoke({"usage-scan", 3}, "scan",
            addon::encode_usage_scan_request(m_request, 3), 30000, m_invocation);
        const auto outcome = m_host.wait_outcome(id, 31000);
        check(outcome.has_value(), "packaged usage scan finishes");
        return *outcome;
    }

    addon::Usage_scan_page legacy_scan()
    {
        auto request = m_request;
        request.cache_selection.reset();
        request.snapshot_id.reset();
        request.cursor.reset();
        const auto id = m_host.invoke({"usage-scan", 2}, "scan",
            addon::encode_usage_scan_request(request, 2), 30000);
        const auto outcome = m_host.wait_outcome(id, 31000);
        check(outcome && outcome->status == addon::Host_status::SUCCESS,
            "legacy packaged usage scan succeeds: " + (outcome ? outcome->reason_code : "missing-outcome"));
        addon::Contract_failure failure;
        const auto page = addon::decode_usage_scan_result(outcome->payload, &failure, 2);
        check(page && page->complete, "legacy packaged usage page decodes: " + failure.detail);
        return *page;
    }

    // Every page of one snapshot, as the host reads them before staging.
    addon::Usage_scan_page scan()
    {
        std::optional<addon::Usage_scan_page> snapshot;
        for (;;) {
            const auto outcome = scan_outcome();
            check(outcome.status == addon::Host_status::SUCCESS, "packaged usage scan succeeds: " + outcome.reason_code);
            check(!addon::account_context_acknowledgment_failure(*m_request.context.account_context, outcome.payload),
                "every page acknowledges the exact account context");
            check(!addon::profile_storage_acknowledgment_failure(*m_request.context.profile_storage, outcome.payload),
                "every page acknowledges the exact profile storage");
            addon::Contract_failure failure;
            auto page = addon::decode_usage_scan_result(outcome.payload, &failure, 3);
            check(page.has_value(), "usage page decodes: " + failure.detail);
            check(page->cache_candidate && page->cache_candidate->root == m_request.cache_selection->candidate_root,
                "every page names the host's candidate allocation");
            if (!snapshot) {
                snapshot = *page;
            }
            else {
                check(page->cache_candidate == snapshot->cache_candidate, "the candidate is identical across pages");
                snapshot->records.insert(snapshot->records.end(), page->records.begin(), page->records.end());
                snapshot->complete = page->complete;
            }
            if (page->complete) {
                break;
            }
            m_request.snapshot_id = page->snapshot_id;
            m_request.cursor      = page->next_cursor;
        }
        return *snapshot;
    }

    bool sealed(const addon::Usage_cache_reference& reference) const
    {
        addon::Contract_failure failure;
        return addon::validate_usage_cache_candidate(reference, m_storage_root, m_request.context.account_context,
            [] { return false; }, &failure).has_value();
    }

    bool candidate_sealed() const
    {
        return fs::exists(addon::usage_cache_io_path(
            addon::path_from_utf8(m_request.cache_selection->candidate_root)) / "cache-manifest.json");
    }

    std::optional<addon::Json_value> import_record(std::string_view kind, const addon::Json_value& record)
    {
        const auto id = m_host.invoke({"legacy-state-import", 1}, addon::k_import_record_operation,
            addon::encode_import_record_request(kind, record), 5000);
        const auto outcome = m_host.wait_outcome(id, 6000);
        check(outcome && outcome->status == addon::Host_status::SUCCESS, "legacy import answers");
        addon::Contract_failure failure;
        const auto result = addon::decode_import_record_result(outcome->payload, &failure);
        check(result.has_value(), "legacy import result decodes: " + failure.detail);
        if (!result->matched()) {
            return std::nullopt;
        }
        return *result->conversion();
    }

private:
    static addon::Simulator_options simulator_options(const fs::path& package, const fs::path& fixture)
    {
        addon::Simulator_options options;
        options.package_directory    = package;
        options.profile_storage_root = fixture / "host-storage";
        return options;
    }

    addon::Host_simulator               m_host;
    std::string                         m_account_key;
    std::string                         m_storage_root;
    addon::Usage_scan_request           m_request;
    addon::Simulator_invocation_options m_invocation;
};

std::map<std::string, addon::Usage_record> records_by_session(const addon::Usage_scan_page& page)
{
    std::map<std::string, addon::Usage_record> sessions;
    for (const auto& record : page.records) {
        check(sessions.emplace(record.record_id, record).second, "one record per session");
    }
    return sessions;
}

std::uint64_t total(const addon::Usage_scan_page& page)
{
    std::uint64_t sum = 0;
    for (const auto& record : page.records) {
        sum += record.total_tokens;
    }
    return sum;
}

// The package's converted message cache inside a sealed candidate: the
// header line names the native seed it was built from, every other line is
// one source file's messages.
std::vector<std::string> cache_lines(const addon::Usage_cache_reference& reference)
{
    const auto bytes = read(addon::usage_cache_io_path(addon::path_from_utf8(reference.root)) / "messages.jsonl");
    std::vector<std::string> lines;
    std::size_t begin = 0;
    while (begin < bytes.size()) {
        const auto end = bytes.find('\n', begin);
        check(end != std::string::npos, "the message cache ends with a newline");
        lines.push_back(bytes.substr(begin, end - begin));
        begin = end + 1;
    }
    check(!lines.empty(), "the message cache has a header");
    return lines;
}

std::string cache_seed_digest(const addon::Usage_cache_reference& reference)
{
    const auto header = addon::parse_json(cache_lines(reference).front(), {}, nullptr);
    const auto* digest = header ? header->find("seed_sha256_hex") : nullptr;
    check(digest && digest->is_string(), "the message cache header names its seed");
    return digest->as_string();
}

// The total of every cached message of one session, in source order.
std::vector<std::uint64_t> cached_message_totals(const addon::Usage_cache_reference& reference, std::string_view session)
{
    std::vector<std::uint64_t> totals;
    const auto lines = cache_lines(reference);
    for (std::size_t index = 1; index < lines.size(); ++index) {
        const auto file = addon::parse_json(lines[index], {}, nullptr);
        const auto* records = file ? file->find("records") : nullptr;
        check(records && records->is_array(), "a cached source file lists its messages");
        for (const auto& message : records->as_array()) {
            const auto* owner = message.find("session");
            const auto* count = message.find("total");
            check(owner && owner->is_string() && count && count->is_integer(), "a cached message has its session and total");
            if (owner->as_string() == session) {
                totals.push_back(static_cast<std::uint64_t>(count->as_integer()));
            }
        }
    }
    std::sort(totals.begin(), totals.end());
    return totals;
}

enum class Third_session
{
    SINGLE_MESSAGE,
    MULTIPLE_MESSAGES,
};

std::string dated_message(std::string_view id, std::string_view instant,
    std::uint64_t input, std::uint64_t output = 0,
    std::uint64_t cache_read = 0, std::uint64_t cache_creation = 0)
{
    auto event = addon::Json_value::object();
    event.set("type", addon::Json_value("assistant"));
    event.set("sessionId", addon::Json_value("temporal-conversation"));
    event.set("timestamp", addon::Json_value(instant));
    auto message = addon::Json_value::object();
    message.set("id", addon::Json_value(id));
    message.set("stop_reason", addon::Json_value("end_turn"));
    auto usage = addon::Json_value::object();
    usage.set("input_tokens", addon::Json_value(input));
    usage.set("output_tokens", addon::Json_value(output));
    usage.set("cache_read_input_tokens", addon::Json_value(cache_read));
    usage.set("cache_creation_input_tokens", addon::Json_value(cache_creation));
    message.set("usage", std::move(usage));
    event.set("message", std::move(message));
    return addon::serialize_json(event) + '\n';
}

void check_temporal_parent(const addon::Usage_scan_page& page, const addon::Usage_record& record,
    std::string_view account)
{
    check(record.temporal && record.temporal->parent.record_id == record.record_id &&
        record.temporal->parent.total_tokens == record.total_tokens &&
        record.temporal->parent.snapshot_id == page.snapshot_id &&
        record.temporal->parent.source_account_key == std::optional<std::string>(account),
        "typed temporal evidence binds the exact accounting parent, source account and actual page snapshot");
    std::uint64_t represented = record.temporal->unavailable_tokens;
    for (const auto& portion : record.temporal->portions) {
        check(portion.basis == "instant" && portion.total_tokens > 0,
            "only positive proved instant portions enter transcript activity");
        represented += portion.total_tokens;
    }
    check(represented == record.total_tokens, "temporal portions and unavailable remainder conserve H");
    check(!addon::encode_usage_record(record, 2).find("capabilities"),
        "usage-scan@2 retains its accounting grammar without temporal evidence");
}

void temporal_messages_and_retained_cache(const fs::path& package, const fs::path& fixture)
{
    const auto profile = fixture / "profile";
    const auto source = source_path(profile, "conversation");
    const auto copy = source_path(profile, "copy");
    const std::string first_instant = "2026-10-08T23:59:59.999999999+02:00";
    const std::string second_instant = "2026-10-10t03:00:00.000000000000000000001-04:00";
    const auto first_message = dated_message("message-first", first_instant, 10, 4, 2, 3);
    write(source, first_message + first_message +
        dated_message("message-second", second_instant, 7, 5) +
        dated_message("message-undated", "2026-10-09T12:00:00", 2) +
        dated_message("message-zero", "2026-10-09T12:00:00Z", 0) +
        "{\"type\":\"assistant\",\"message\":{\"id\":\"unfinished\",\"stop_reason\":null}}\n" +
        "{\"type\":\"assistant\",\"message\":{");
    write(copy, first_message);
    Packaged_host host(package, fixture, profile);
    host.select("observed", std::nullopt);
    const auto observed = host.scan();
    check(observed.records.size() == 1 && total(observed) == 33,
        "finalized stable IDs deduplicate before conversation reduction and exclude unfinished records");
    const auto& record = observed.records.front();
    check_temporal_parent(observed, record, host.account_key());
    check(record.temporal->portions.size() == 2 && record.temporal->unavailable_tokens == 2 &&
        record.temporal->coverage_reason == "missing-evidence" &&
        record.temporal->portions[0].instant == first_instant && record.temporal->portions[0].total_tokens == 19 &&
        record.temporal->portions[1].instant == second_instant && record.temporal->portions[1].total_tokens == 12 &&
        record.completed_date >= "2026-10-09" && record.input.value == 19 && record.output.value == 9 &&
        record.cache_read.value == 2 && record.cache_creation.value == 3,
        "earlier activity survives a later parent date, raw offset/fraction precision and original accounting partition");
    const auto original_lines = cache_lines(*observed.cache_candidate);
    check(fs::remove(source) && fs::remove(copy), "delete only isolated original transcripts");
    host.select("deleted-originals", observed.cache_candidate);
    const auto retained = host.scan();
    check_temporal_parent(retained, retained.records.front(), host.account_key());
    check(retained.records.front().temporal->portions.size() == 2 &&
        retained.records.front().temporal->portions[0].instant == first_instant &&
        retained.records.front().temporal->retained_from &&
        retained.records.front().temporal->retained_from->snapshot_id == observed.snapshot_id &&
        retained.records.front().temporal->retained_from->manifest_sha256_hex ==
            observed.cache_candidate->manifest_sha256_hex && total(retained) == 33,
        "selected accepted cache retains deleted-source instants with exact prior snapshot and manifest custody");

    const auto cache_fixture = [&](std::string name, const auto& change,
        std::optional<addon::Account_context> account = std::nullopt)
    {
        const auto root = host.storage_root() / ("evidence-" + name);
        check(fs::create_directory(root), "allocate isolated optional-cache fixture");
        std::string bytes = original_lines.front() + '\n';
        for (std::size_t index = 1; index < original_lines.size(); ++index) {
            auto wrapper = addon::parse_json(original_lines[index], {}, nullptr);
            check(wrapper.has_value(), "parse isolated cached accounting");
            for (auto& message : wrapper->find("records")->as_array()) change(message);
            bytes += addon::serialize_json(*wrapper) + '\n';
        }
        write(root / "messages.jsonl", bytes);
        addon::Contract_failure failure;
        const auto reference = addon::seal_usage_cache_candidate(addon::path_to_utf8(root),
            addon::path_to_utf8(host.storage_root()), account ? account :
                std::optional<addon::Account_context>{{2, host.account_key(), addon::path_to_utf8(profile)}},
            "fixture-" + name, "claude-message-cache@2", [] { return false; }, &failure);
        check(reference.has_value(), "seal isolated accounting fixture: " + failure.detail);
        return *reference;
    };
    const auto replay = [&](std::string name, const auto& change, std::uint64_t unavailable,
        std::string_view reason)
    {
        const auto accepted = cache_fixture(name, change);
        host.select("replay-" + name, accepted);
        const auto page = host.scan();
        check(page.records.size() == 1 && total(page) == 33, "optional damage preserves admitted accounting: " + name);
        check_temporal_parent(page, page.records.front(), host.account_key());
        check(page.records.front().temporal->unavailable_tokens == unavailable &&
            page.records.front().temporal->coverage_reason == reason,
            "optional carrier withdrawal is truthful: " + name);
        check(host.sealed(accepted), "replay leaves selected accepted candidate immutable");
        return page;
    };
    replay("date-only", [](auto& message) { message.erase("usage-temporal"); }, 33, "missing-evidence");
    replay("bad-digest", [](auto& message) {
        if (auto* carrier = message.find("usage-temporal")) {
            carrier->set("parent_sha256_hex", addon::Json_value(std::string(64, '0')));
        }
    }, 33, "missing-evidence");
    replay("malformed", [](auto& message) {
        if (message.find("usage-temporal")) message.set("usage-temporal", addon::Json_value("damaged"));
    }, 33, "missing-evidence");
    auto future = addon::Json_value::object();
    future.set("version", addon::Json_value(9));
    future.set("future_evidence", addon::Json_value("retained opaque evidence"));
    const auto future_page = replay("future", [&future](auto& message) {
        if (message.find("usage-temporal")) message.set("usage-temporal", future);
    }, 33, "missing-evidence");
    const auto future_lines = cache_lines(*future_page.cache_candidate);
    const auto future_wrapper = addon::parse_json(future_lines[1], {}, nullptr);
    check(*future_wrapper->find("records")->as_array().front().find("usage-temporal") == future,
        "unknown independently versioned carrier remains opaque in candidate cache replay");
    replay("oversize", [](auto& message) {
        if (auto* carrier = message.find("usage-temporal")) {
            carrier->set("instant", addon::Json_value(std::string(addon::k_max_usage_temporal_bytes + 1, 'x')));
        }
    }, 33, "missing-evidence");
    const auto corrected_cache = cache_fixture("corrected-accounting", [](auto& message) {
        if (message.find("id")->as_string() == "message-first") {
            message.set("total", addon::Json_value(20));
            message.find("categories")->set("input_tokens", addon::Json_value(11));
        }
    });
    host.select("corrected-accounting", corrected_cache);
    const auto corrected = host.scan();
    check_temporal_parent(corrected, corrected.records.front(), host.account_key());
    check(total(corrected) == 34 && corrected.records.front().input.value == 20 &&
        corrected.records.front().temporal->portions.size() == 1 &&
        corrected.records.front().temporal->portions.front().instant == second_instant &&
        corrected.records.front().temporal->unavailable_tokens == 22,
        "an accounting correction cannot inherit the prior message's stale temporal claim");

    auto wrong_snapshot = *observed.cache_candidate;
    wrong_snapshot.snapshot_id = "another-snapshot";
    host.select("wrong-snapshot", wrong_snapshot);
    check(host.scan_outcome().status != addon::Host_status::SUCCESS,
        "accepted cache cannot be grafted from a different snapshot reference");
    const auto wrong_account = cache_fixture("wrong-account", [](auto&) {},
        addon::Account_context{2, "other-account", addon::path_to_utf8(profile)});
    host.select("wrong-account", wrong_account);
    check(host.scan_outcome().status != addon::Host_status::SUCCESS,
        "accepted cache cannot be grafted from another source account");
    check(host.sealed(*observed.cache_candidate), "failed custody checks preserve the original accepted cache");
    host.finish();
}

void temporal_duplicate_and_capacity_contract(const fs::path& package, const fs::path& fixture)
{
    const auto profile = fixture / "profile";
    const auto source = source_path(profile, "conversation");
    const auto copy = source_path(profile, "copy");
    write(source, dated_message("same-id", "2026-10-08T12:00:00Z", 17));
    write(copy, dated_message("same-id", "2026-10-09T12:00:00Z", 17));
    Packaged_host host(package, fixture, profile);
    host.select("conflicting-instants", std::nullopt);
    const auto conflict = host.scan();
    check_temporal_parent(conflict, conflict.records.front(), host.account_key());
    check(total(conflict) == 17 && conflict.records.front().temporal->portions.empty() &&
        conflict.records.front().temporal->unavailable_tokens == 17 &&
        conflict.records.front().temporal->coverage_reason == "unsupported",
        "differing instants on the same admitted message ID withdraw temporal without duplicating accounting");
    write(copy, dated_message("same-id", "2026-10-09T12:00:00Z", 18));
    host.select("conflicting-totals", std::nullopt);
    const auto accounting_conflict = host.scan_outcome();
    check(accounting_conflict.status != addon::Host_status::SUCCESS &&
        accounting_conflict.reason_code == "conflicting_message_usage",
        "differing H on duplicate IDs retains the original accounting refusal");
    check(fs::remove(copy), "remove only isolated duplicate transcript");
    std::string bytes;
    for (unsigned index = 0; index <= addon::k_max_usage_temporal_portions; ++index) {
        bytes += dated_message("position-" + std::to_string(index),
            "2026-10-09T12:00:00." + std::to_string(100000 + index) + 'Z', 1);
    }
    write(source, bytes);
    host.select("optional-capacity", std::nullopt);
    const auto capacity = host.scan();
    check_temporal_parent(capacity, capacity.records.front(), host.account_key());
    check(total(capacity) == addon::k_max_usage_temporal_portions + 1 &&
        capacity.records.front().temporal->portions.empty() &&
        capacity.records.front().temporal->unavailable_tokens == total(capacity) &&
        capacity.records.front().temporal->coverage_reason == "capacity",
        "optional position capacity preserves every admitted accounting message without a history ceiling");
    bytes.clear();
    for (unsigned index = 0; index <= addon::k_max_usage_temporal_portions; ++index) {
        bytes += dated_message("position-" + std::to_string(index),
            "2026-10-09T12:00:00." + std::to_string(100000 + index) + 'Z',
            index == addon::k_max_usage_temporal_portions ? 0 : 1);
    }
    write(source, bytes);
    host.select("zero-beyond-position-bound", std::nullopt);
    const auto zero_position = host.scan();
    check_temporal_parent(zero_position, zero_position.records.front(), host.account_key());
    check(total(zero_position) == addon::k_max_usage_temporal_portions &&
        zero_position.records.front().temporal->portions.size() == addon::k_max_usage_temporal_portions &&
        zero_position.records.front().temporal->unavailable_tokens == 0,
        "a zero-token message consumes no positive position capacity");
    write(source, dated_message("zero", "2026-10-09T12:00:00Z", 0));
    host.select("zero-parent", std::nullopt);
    const auto zero = host.scan();
    check_temporal_parent(zero, zero.records.front(), host.account_key());
    check(total(zero) == 0 && zero.records.front().temporal->portions.empty(),
        "H zero produces no positive temporal portion");
    write(source, dated_message("compact-offset", "2026-10-09T12:00:00+0000", 3) +
        dated_message("unknown-offset", "2026-10-09T12:00:00-00:00", 5));
    host.select("unsupported-native-instants", std::nullopt);
    const auto unsupported = host.scan();
    check_temporal_parent(unsupported, unsupported.records.front(), host.account_key());
    check(total(unsupported) == 8 && unsupported.records.front().temporal->portions.empty() &&
        unsupported.records.front().temporal->unavailable_tokens == 8,
        "broader accounting date admission cannot manufacture strict RFC3339 instant proof");
    host.finish();
}

void optional_parent_grammar_contract(const fs::path& package, const fs::path& fixture)
{
    const auto profile = fixture / "profile";
    const std::string session_id = "control-" + std::string(1, '\x01') + "-parent";
    auto event = addon::parse_json(dated_message("control-message", "2026-10-09T12:00:00Z", 11, 6), {}, nullptr);
    check(event.has_value(), "parse isolated optional-parent fixture");
    event->set("sessionId", addon::Json_value(session_id));
    write(source_path(profile, "conversation"), addon::serialize_json(*event) + '\n');
    Packaged_host host(package, fixture, profile);
    const auto legacy = host.legacy_scan();
    check(legacy.records.size() == 1 && legacy.records.front().record_id == session_id &&
        legacy.records.front().session_key == session_id && total(legacy) == 17,
        "usage2 preserves the admitted nonNUL control-character parent ID and exact H");
    host.select("optional-parent", std::nullopt);
    const auto current = host.scan();
    check(current.records.size() == 1 && current.records.front().record_id == session_id &&
        current.records.front().session_key == session_id && total(current) == 17 &&
        !current.records.front().temporal && host.sealed(*current.cache_candidate),
        "usage3 omits invalid optional parent proof and preserves the same accounting ID, H and sealed cache");
    host.finish();
}

enum class Reimport_seed
{
    RETAINED,
    CHANGED,
};

// addon_usage_native_rollback_packaged_claude, package half.
void retained_history_survives_rollback_and_reimport(
    const fs::path& package,
    const fs::path& fixture,
    Third_session   third,
    Reimport_seed   reimport)
{
    const auto profile = fixture / "native-claude";
    fs::create_directories(profile / "projects" / "repo");
    Packaged_host host(package, fixture, profile);

    const addon::Legacy_account_record account{"claude", "native-account", addon::path_to_utf8(profile)};
    const auto resolved = host.import_record(addon::k_legacy_account_record_kind,
        addon::encode_legacy_account_record(account));
    const auto* resolved_key = resolved ? resolved->find("adapter_account_key") : nullptr;
    check(resolved_key && resolved_key->is_string() && resolved_key->as_string() == host.account_key(),
        "the native account maps to the profile's add-on account");

    // The host converts every retained record's identity before an import and
    // back again on rollback. Claude history is keyed by its session, so a
    // retained session keeps its identity both ways.
    for (const std::string direction : {"to-add-on", "to-legacy"}) {
        addon::Legacy_usage_records records{direction, "claude", "native-account", host.account_key(), {}};
        std::uint16_t position = 0;
        for (const std::string session : {"record-one", "record-two", "record-three"}) {
            records.records.push_back({position++, session, session, host_path(source_path(profile, session))});
        }
        const auto converted = host.import_record(addon::k_legacy_usage_records_kind,
            addon::encode_legacy_usage_records(records));
        check(converted.has_value(), direction + " conversion matches the retained sessions");
        addon::Contract_failure failure;
        const auto identities = addon::decode_legacy_usage_conversion(*converted, direction, &failure);
        check(identities && identities->size() == records.records.size(), direction + " conversion covers every record");
        for (std::size_t index = 0; index < identities->size(); ++index) {
            check((*identities)[index].record_id == records.records[index].record_id,
                direction + " conversion keeps the retained session identity");
        }
    }

    // Retained native history whose transcripts no longer exist.
    const Retained_message one{"record-one", "record-one-message", 60};
    const Retained_message two{"record-two", "record-two-message", 40};
    const std::string seed = retained_file_seed_line(profile, one) + retained_file_seed_line(profile, two);
    host.select("import", std::nullopt);
    const auto seed_digest = host.seed("native-file-cache-import.jsonl", seed, true);
    const auto imported = host.scan();
    const auto imported_sessions = records_by_session(imported);
    check(imported_sessions.size() == 2 && total(imported) == 100, "the import converts every retained session");
    for (const auto& retained : {one, two}) {
        const auto& record = imported_sessions.at(retained.session);
        check(record.total_tokens == retained.tokens &&
            record.source_path == std::optional<std::string>(host_path(source_path(profile, retained.session))) &&
            record.source_counter_semantics == std::optional<std::string>("per_event_sum") &&
            record.aggregate_evidence == std::optional<std::string>("partial"),
            "an imported session keeps its retained count and provenance: " + retained.session);
    }
    check(host.sealed(*imported.cache_candidate) && cache_seed_digest(*imported.cache_candidate) == seed_digest,
        "the imported cache is sealed and names the seed it converted");

    // Reopening reconciles from the accepted converted cache; the native
    // transcripts are gone and are not rediscovered.
    host.select("reconcile", imported.cache_candidate);
    const auto reconciled = host.scan();
    check(total(reconciled) == 100, "the accepted converted cache is replayed");

    // A session added after the import joins the accepted history.
    const auto third_path = source_path(profile, "record-three");
    if (third == Third_session::SINGLE_MESSAGE) {
        write(third_path, transcript_message("record-three", "record-three-message", 25));
    }
    else {
        write(third_path, transcript_message("record-three", "record-three-message-0", 10) +
            transcript_message("record-three", "record-three-message-1", 15));
    }
    host.select("third-session", reconciled.cache_candidate);
    const auto extended = host.scan();
    check(total(extended) == 125, "the new session joins the accepted history");
    const auto extended_sessions = records_by_session(extended);
    const auto accepted_third_path = extended_sessions.at("record-three").source_path;
    check(extended_sessions.at("record-three").total_tokens == 25 && accepted_third_path.has_value(),
        "the new session carries its transcript");
    check(cache_lines(*extended.cache_candidate).size() == 4, "the accepted cache holds all three source files");
    const std::vector<std::uint64_t> expected_third = third == Third_session::SINGLE_MESSAGE
        ? std::vector<std::uint64_t>{25}
        : std::vector<std::uint64_t>{10, 15};
    check(cached_message_totals(*extended.cache_candidate, "record-three") == expected_third,
        "the accepted cache keeps every message of the new session");

    // After the rollback the transcript is gone too. The host imports again
    // from the native seed it retained, with the cache it accepted before the
    // rollback selected.
    check(fs::remove(third_path), "remove the third transcript");
    host.select("reimport", extended.cache_candidate);
    if (reimport == Reimport_seed::RETAINED) {
        host.seed("native-file-cache-reimport.jsonl", seed, true);
        const auto reimported = host.scan();
        check(total(reimported) == 125, "the second import keeps every accepted session");
        check(records_by_session(reimported).at("record-three").source_path == accepted_third_path,
            "the second import keeps the accepted transcript of the new session");
        const auto accepted_lines = cache_lines(*extended.cache_candidate);
        const auto reimported_lines = cache_lines(*reimported.cache_candidate);
        check(std::equal(accepted_lines.begin() + 1, accepted_lines.end(),
                reimported_lines.begin() + 1, reimported_lines.end()),
            "the second import reproduces the accepted message cache");
        check(host.sealed(*reimported.cache_candidate), "the second import is sealed");
    }
    else {
        // Native evidence that changed while the add-on was absent no longer
        // matches the seed the accepted cache was built from, so the import
        // fails instead of mixing the two histories.
        Retained_message changed = one;
        changed.message_id = "changed-retained-message";
        const auto changed_digest = host.seed("native-file-cache-reimport.jsonl",
            retained_file_seed_line(profile, changed) + retained_file_seed_line(profile, two), true);
        check(changed_digest != cache_seed_digest(*extended.cache_candidate),
            "the changed seed differs from the one the accepted cache names");
        const auto refused = host.scan_outcome();
        check(refused.status == addon::Host_status::FAILED &&
            refused.reason_code == "usage_seed_receipt_mismatch" &&
            refused.message.find("does not match the saved cache receipt") != std::string::npos,
            "a seed that contradicts the accepted cache fails the import: " + refused.reason_code);
        check(!host.candidate_sealed(), "a refused import seals no candidate");
        check(host.sealed(*extended.cache_candidate), "a refused import leaves the accepted cache intact");
    }
    host.finish();
}

} // namespace

int main(int argc, char** argv)
{
    try {
        check(argc == 2, "usage host tests require the built package directory");
        const auto package = addon::path_from_utf8(argv[1]);
        Temporary_directory fixture;
        retained_history_survives_rollback_and_reimport(package, fixture.path() / "single-message",
            Third_session::SINGLE_MESSAGE, Reimport_seed::RETAINED);
        retained_history_survives_rollback_and_reimport(package, fixture.path() / "multiple-messages",
            Third_session::MULTIPLE_MESSAGES, Reimport_seed::RETAINED);
        retained_history_survives_rollback_and_reimport(package, fixture.path() / "changed-native-cache",
            Third_session::SINGLE_MESSAGE, Reimport_seed::CHANGED);
        optional_parent_grammar_contract(package, fixture.path() / "optional-parent-grammar");
        temporal_messages_and_retained_cache(package, fixture.path() / "temporal-cache");
        temporal_duplicate_and_capacity_contract(package, fixture.path() / "temporal-admission");
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
