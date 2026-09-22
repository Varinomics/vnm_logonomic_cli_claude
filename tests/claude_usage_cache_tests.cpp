#include "claude_accounts.h"
#include "claude_usage.h"

#include <logonomic_cli_addon/environment_bootstrap.h>
#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/profile_storage.h>
#include <logonomic_cli_addon/usage_scan.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <utility>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

void check(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class Cancelled_context final : public addon::Invocation_context
{
public:
    explicit Cancelled_context(const addon::Usage_scan_request& input)
    {
        m_request.capability = {"usage-scan", 3};
        m_request.operation = "scan";
        m_request.payload = addon::encode_usage_scan_request(input, 3);
    }

    const addon::Invoke_request& request() const override { return m_request; }
    bool cancel_requested() const override { return true; }
    bool wait_for_cancel(int) override { return true; }
    void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override {}
    void succeed(addon::Json_value) override { throw std::runtime_error("cancelled scan succeeded"); }
    void fail(addon::Result_status, std::string reason, std::string) override
    {
        throw std::runtime_error(reason);
    }
    bool finished() const override { return false; }

private:
    addon::Invoke_request m_request;
};

class Scan_context final : public addon::Invocation_context
{
public:
    const addon::Invoke_request& request() const override { return m_request; }
    bool cancel_requested() const override { return false; }
    bool wait_for_cancel(int) override { return false; }
    void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override {}
    void succeed(addon::Json_value) override {}
    void fail(addon::Result_status, std::string, std::string) override {}
    bool finished() const override { return false; }

private:
    addon::Invoke_request m_request;
};

std::string assistant_record(std::string_view session, std::string_view id, unsigned tokens)
{
    return "{\"type\":\"assistant\",\"sessionId\":\"" + std::string(session) +
        "\",\"timestamp\":\"2026-09-12T12:00:00Z\",\"message\":{\"id\":\"" + std::string(id) +
        "\",\"stop_reason\":\"end_turn\",\"usage\":{\"input_tokens\":" + std::to_string(tokens) + "}}}\n";
}

// A live session appends to its transcript while a scan reads it. This
// context appends one completed record at each cancellation poll, a bounded
// number of times.
class Live_writer_context final : public addon::Invocation_context
{
public:
    explicit Live_writer_context(fs::path transcript)
    :
        m_transcript(std::move(transcript))
    {}

    const addon::Invoke_request& request() const override { return m_request; }
    bool cancel_requested() const override
    {
        if (m_appended < 20) {
            std::ofstream append(m_transcript, std::ios::binary | std::ios::app);
            append << assistant_record("live", "live-appended-" + std::to_string(m_appended), 7);
            append.close();
            check(append.good(), "live transcript append written");
            ++m_appended;
        }
        return false;
    }
    bool wait_for_cancel(int) override { return false; }
    void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override {}
    void succeed(addon::Json_value) override {}
    void fail(addon::Result_status, std::string, std::string) override {}
    bool finished() const override { return false; }

    unsigned appended() const { return m_appended; }

private:
    addon::Invoke_request m_request;
    fs::path              m_transcript;
    mutable unsigned      m_appended = 0;
};

class Workspace
{
public:
    Workspace()
    :
        m_path(fs::temp_directory_path() / ("claude-cache-contract-" + addon::random_nonce_hex()))
    {
        fs::create_directories(m_path);
        m_path = fs::canonical(m_path);
    }

    ~Workspace()
    {
        std::error_code error;
        fs::remove_all(m_path, error);
    }

    fs::path m_path;
};

void write(const fs::path& path, const std::string& bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << bytes;
    output.close();
    check(output.good(), "local telemetry fixture write failed");
}

std::vector<fs::path> write_sources(const fs::path& profile, unsigned tokens)
{

    std::string history;
    for (unsigned index = 0; index < 270; ++index) {
        const auto id = "session-" + std::to_string(index);
        history += "{\"type\":\"assistant\",\"sessionId\":\"" + id +
            "\",\"timestamp\":\"2026-09-12T12:00:00Z\",\"message\":{\"id\":\"" + id +
            "\",\"stop_reason\":\"end_turn\",\"usage\":{\"input_tokens\":" +
            std::to_string(tokens) + ",\"output_tokens\":0,\"cache_read_input_tokens\":0,\"cache_creation_input_tokens\":0}}}\n";
    }
    const auto path = profile / "projects/repository/messages.jsonl";
    write(path, history);
    return {path};
}

void adapter_cache_contract(const fs::path& package, const fs::path& fixture)
{
    const auto profile = fixture / "native-profile";
    auto sources = write_sources(profile, 10);
    const auto native_profile = addon::path_to_utf8(profile);
    const auto account = claude::account_key(profile);

    addon::Simulator_options options;
    options.package_directory = package;
    options.profile_storage_root = fixture / "host-storage";
    addon::Host_simulator host(options, nullptr);
    addon::Contract_failure failure;
    check(host.start(&failure), failure.detail);
    std::string error;
    const auto allocated = addon::allocate_profile_storage_root(
        host.profile_storage_root(), "logonomic.cli.claude", &error, account);
    check(allocated.has_value(), error);
    const auto storage = addon::path_from_utf8(*allocated);

    addon::Usage_scan_request request;
    request.context.launcher = {"claude", addon::k_runner_direct, {}};
    request.context.launcher_argv = {"claude"};
    request.context.profile_storage = addon::Profile_storage_context{1, *allocated};
    request.context.account_context = addon::Account_context{2, account, native_profile};
    request.context.launch_environment = addon::Launch_environment{
        addon::host_platform_id(), addon::Json_value::object(),
        addon::Environment_bootstrap_evidence{native_profile}};

    addon::Simulator_invocation_options invocation;
    invocation.profile_storage = request.context.profile_storage;
    const auto acknowledge = host.invoke({"profile-storage", 1}, "acknowledge",
        addon::Json_value::object(), 5000, invocation);
    const auto acknowledged = host.wait_outcome(acknowledge, 6000);
    check(acknowledged && acknowledged->status == addon::Host_status::SUCCESS,
        "packaged adapter acknowledges allocated storage");

    const auto outcome_for = [&](const addon::Usage_scan_request& input, std::uint16_t version) {
        const auto id = host.invoke({"usage-scan", version}, "scan",
            addon::encode_usage_scan_request(input, version), 30000);
        const auto outcome = host.wait_outcome(id, 31000);
        check(outcome.has_value(), "packaged scan must finish");
        return *outcome;
    };
    const auto page_for = [&](const addon::Usage_scan_request& input, std::uint16_t version) {
        const auto outcome = outcome_for(input, version);
        check(outcome.status == addon::Host_status::SUCCESS,
            "packaged scan failed: " + outcome.reason_code + "; request=" +
                addon::serialize_json(addon::encode_usage_scan_request(input, version)));
        const auto page = addon::decode_usage_scan_result(outcome.payload, &failure, version);
        check(page.has_value(), failure.detail);
        if (version == 3) {
            check(page->profile_storage && input.context.profile_storage &&
                page->profile_storage->version == input.context.profile_storage->version &&
                page->profile_storage->root == input.context.profile_storage->root,
                "every usage3 page echoes the exact requested profile storage");
            check(page->account_context.has_value() == input.context.account_context.has_value(),
                "usage3 preserves account-context presence without synthesizing it");
            if (input.context.account_context) {
                check(addon::encode_account_context(*page->account_context) ==
                    addon::encode_account_context(*input.context.account_context),
                    "every usage3 page preserves account version, key and profile exactly");
            }
            else {
                check(!outcome.payload.find("account_context"),
                    "absent account context is omitted on the wire, not null");
            }
        }
        else {
            check(!outcome.payload.find("account_context") && !outcome.payload.find("profile_storage"),
                "legacy usage pages do not acquire usage3 acknowledgments");
        }
        return *page;
    };
    const auto all_pages = [&](addon::Usage_scan_request input, std::uint16_t version) {
        auto first = page_for(input, version);
        auto page = first;
        while (!page.complete) {
            input.snapshot_id = page.snapshot_id;
            input.cursor = page.next_cursor;
            page = page_for(input, version);
            check(page.cache_candidate == first.cache_candidate,
                "candidate descriptor stays identical across pages");
            first.records.insert(first.records.end(), page.records.begin(), page.records.end());
        }
        return first;
    };
    const auto total = [](const addon::Usage_scan_page& page) {
        std::uint64_t sum = 0;
        for (const auto& record : page.records) {
            sum += record.total_tokens;
        }
        return sum;
    };
    const auto select = [&](std::string name, std::optional<addon::Usage_cache_reference> accepted) {
        request.snapshot_id.reset();
        request.cursor.reset();
        request.mode = "incremental";
        auto parent = storage;
        while (parent.native().size() < 291) {
            parent /= "retained-account-cache-allocation-component";
        }
        const auto path = parent / name;
        check(fs::create_directories(addon::usage_cache_io_path(path)),
            "host exclusively allocates long candidate directory");
        request.cache_selection = addon::Usage_cache_selection{std::move(accepted), addon::path_to_utf8(path)};
    };
    const auto validate = [&](const addon::Usage_cache_reference& reference) {
        check(addon::validate_usage_cache_candidate(reference, *allocated, request.context.account_context,
            [] { return false; }, &failure).has_value(), "sealed candidate remains intact: " + failure.detail);
    };

    check(total(all_pages(request, 2)) == 2700, "v2 retains its existing namespace and telemetry");
    for (const auto& path : sources) {
        check(fs::remove(path), "remove fixture source");
    }
    select("empty-selection", std::nullopt);
    const auto empty = all_pages(request, 3);
    check(empty.records.empty() && empty.cache_candidate,
        "absent host selection never bootstraps from mutable provider cache");
    validate(*empty.cache_candidate);

    sources = write_sources(profile, 10);
    select("accepted", std::nullopt);
    const auto first_request = request;
    const auto first = page_for(request, 3);
    check(!first.complete && first.records.size() == 256 && first.cache_candidate,
        "packaged first page supplies a sealed bounded candidate");
    validate(*first.cache_candidate);
    request.snapshot_id = first.snapshot_id;
    request.cursor = first.next_cursor;
    auto changed = request;
    changed.mode = "repair";
    check(outcome_for(changed, 3).status != addon::Host_status::SUCCESS,
        "continuation cannot change the frozen request");
    const auto last = page_for(request, 3);
    check(last.complete && last.records.size() == 14 && last.cache_candidate == first.cache_candidate,
        "packaged final page preserves candidate identity");
    for (const auto& record : first.records) {
        check(record.aggregate_evidence == "partial", "surviving native subsets make conservative evidence claims");
    }

    select("account-v1", std::nullopt);
    request.context.account_context->version = 1;
    request.context.account_context->profile_path.reset();
    // Version one has no profile locator; acknowledgment support must not
    // bypass the native provider's existing explicit-account admission.
    const auto version_one = outcome_for(request, 3);
    check(version_one.status != addon::Host_status::SUCCESS &&
        version_one.reason_code == "claude_operation_failed",
        "version-one account without a profile locator retains its explicit refusal");
    request.context = first_request.context;
    validate(*first.cache_candidate);

    select("account-absent", std::nullopt);
    request.context.account_context.reset();
    request.context.launch_environment->effective_environment.set("CLAUDE_CONFIG_DIR", addon::Json_value(native_profile));
    const auto account_absent = all_pages(request, 3);
    check(account_absent.records.size() == 270 && total(account_absent) == 2700,
        "environment-selected profile keeps account context absent on every page");
    request.context = first_request.context;

    sources = write_sources(profile, 1);
    select("unaccepted", first.cache_candidate);
    request.mode = "repair";
    const auto lower = all_pages(request, 3);
    check(total(lower) == 270 && lower.cache_candidate, "lower native observation is a separate candidate");
    validate(*first.cache_candidate);
    for (const auto& path : sources) {
        check(fs::remove(path), "remove refreshed fixture source");
    }

    // The host rejects the lower partial aggregate and continues selecting the coherent original cache.
    select("recovered", first.cache_candidate);
    const auto recovered = all_pages(request, 3);
    check(total(recovered) == 2700, "rejected candidate cannot become retained history after source deletion");
    validate(*first.cache_candidate);
    validate(*lower.cache_candidate);

    select("cancelled", first.cache_candidate);
    Cancelled_context cancelled(request);
    bool saw_cancel = false;
    try {
        claude::handle_usage(cancelled);
    }
    catch (const std::runtime_error& error) {
        saw_cancel = std::string(error.what()) == "cancelled";
    }
    check(saw_cancel, "cancelled scan cannot seal or promote a candidate");
    check(!fs::exists(addon::path_from_utf8(request.cache_selection->candidate_root) / "cache-manifest.json"),
        "cancelled candidate stays unsealed");
    validate(*first.cache_candidate);

    select("reset", std::nullopt);
    request.mode = "reset";
    const auto reset = all_pages(request, 3);
    check(reset.records.empty() && reset.cache_candidate, "reset reconstructs only current sources");
    validate(*reset.cache_candidate);
    validate(*first.cache_candidate);
    auto invalid_reset = request;
    invalid_reset.cache_selection->accepted = first.cache_candidate;
    check(outcome_for(invalid_reset, 3).status != addon::Host_status::SUCCESS,
        "reset cannot import a retained selection");

    auto old_request = first_request;
    old_request.cache_selection.reset();
    check(total(all_pages(old_request, 2)) == 2700, "v3 scans and reset leave v2 cache untouched");

    select("occupied", first.cache_candidate);
    write(addon::usage_cache_io_path(addon::path_from_utf8(request.cache_selection->candidate_root) / "foreign"),
        "occupied");
    check(outcome_for(request, 3).status != addon::Host_status::SUCCESS,
        "provider refuses a nonempty host allocation");
    validate(*first.cache_candidate);

    const auto mutable_path = storage / "claude-message-cache-v1.jsonl";
    const auto conflicting = mutable_path / "host-candidate";
    if (!fs::exists(mutable_path) || fs::is_directory(mutable_path)) {
        fs::create_directories(conflicting);
    }
    request.cache_selection = addon::Usage_cache_selection{std::nullopt, addon::path_to_utf8(conflicting)};
    check(outcome_for(request, 3).status != addon::Host_status::SUCCESS,
        "host candidates cannot enter the provider mutable namespace");

    select("wrong-account", first.cache_candidate);
    auto wrong_account = request;
    const auto other_profile = fixture / "other-native-profile";
    fs::create_directories(other_profile);
    wrong_account.context.account_context = addon::Account_context{
        2, claude::account_key(other_profile), addon::path_to_utf8(other_profile)};
    wrong_account.context.launch_environment->bootstrap =
        addon::Environment_bootstrap_evidence{addon::path_to_utf8(other_profile)};
    check(outcome_for(wrong_account, 3).status != addon::Host_status::SUCCESS,
        "selected cache cannot cross native profile and account ownership");
    validate(*first.cache_candidate);

    auto temporary_conflict = storage / "claude-cache-host.tmp";
    while (temporary_conflict.native().size() < 291) {
        temporary_conflict /= "retained-account-cache-allocation-component";
    }
    check(fs::create_directories(addon::usage_cache_io_path(temporary_conflict)),
        "allocate long descendant of the root-owned mutable temporary namespace");
    request.cache_selection = addon::Usage_cache_selection{std::nullopt, addon::path_to_utf8(temporary_conflict)};
    check(outcome_for(request, 3).status != addon::Host_status::SUCCESS,
        "host allocation cannot alias the mutable temporary-file namespace");

    select("corrupt-input", first.cache_candidate);
    write(addon::usage_cache_io_path(addon::path_from_utf8(first.cache_candidate->root) / "unlisted"), "unexpected");
    check(outcome_for(request, 3).status != addon::Host_status::SUCCESS,
        "corrupt selected inventory fails without fallback");

    // An incremental scan reuses the accepted evidence of a transcript whose
    // size and modification time are unchanged, and rereads only the others.
    request.context = first_request.context;
    const auto steady  = profile / "projects/incremental/steady.jsonl";
    const auto growing = profile / "projects/incremental/growing.jsonl";
    const auto message = assistant_record;
    const auto session_total = [](const addon::Usage_scan_page& page, std::string_view session) {
        std::uint64_t sum = 0;
        for (const auto& record : page.records) {
            if (record.session_key == session) {
                sum += record.total_tokens;
            }
        }
        return sum;
    };
    write(steady, message("steady", "steady-1", 100) + message("steady", "steady-2", 200) +
        message("steady", "steady-3", 300));
    const auto third = message("growing", "growing-3", 30);
    const auto split = third.size() / 2;
    write(growing, message("growing", "growing-1", 10) + message("growing", "growing-2", 20) +
        third.substr(0, split));
    select("incremental-base", std::nullopt);
    const auto base = all_pages(request, 3);
    check(session_total(base, "steady") == 600 && session_total(base, "growing") == 30,
        "a record still being written is not counted");
    validate(*base.cache_candidate);

    // Unreadable bytes behind an unchanged size and modification time prove
    // that the incremental scan did not parse the file again.
    const auto steady_size     = fs::file_size(steady);
    const auto steady_modified = fs::last_write_time(steady);
    write(steady, std::string(steady_size - 1, 'x') + "\n");
    fs::last_write_time(steady, steady_modified);
    check(fs::file_size(steady) == steady_size && fs::last_write_time(steady) == steady_modified,
        "rewritten fixture keeps its size and modification time");
    {
        std::ofstream append(growing, std::ios::binary | std::ios::app);
        append << third.substr(split) << message("growing", "growing-1", 10) << message("growing", "growing-4", 40);
        append.close();
        check(append.good(), "appended transcript fixture written");
    }
    select("incremental-next", base.cache_candidate);
    const auto next = all_pages(request, 3);
    check(session_total(next, "steady") == 600, "an unchanged transcript is not parsed again");
    check(session_total(next, "growing") == 100,
        "an appended transcript adds exactly its new records, once each");
    validate(*next.cache_candidate);

    select("incremental-repair", next.cache_candidate);
    request.mode = "repair";
    check(outcome_for(request, 3).status != addon::Host_status::SUCCESS,
        "repair rereads every transcript instead of trusting its recorded fingerprint");
    check(host.shutdown(1000, nullptr), "packaged adapter exits without contacting a model");
}

void large_history_contract(const fs::path& fixture)
{
    // Only a single record is bounded, so a history of any size scans. The
    // fixture's history exceeds 512 MiB, the largest record the scanner parses.
    const auto profile = fixture / "large-profile";
    const auto filler = "{\"type\":\"user\",\"sessionId\":\"large-session\",\"timestamp\":\"2026-09-12T12:00:00Z\","
        "\"message\":{\"role\":\"user\",\"content\":\"" + std::string(1024 * 1024, 'x') + "\"}}\n";
    std::uint64_t history_bytes = 0;
    std::uint64_t expected      = 0;
    for (unsigned file = 0; file < 5; ++file) {
        const auto path = profile / "projects/large" / ("session-" + std::to_string(file) + ".jsonl");
        fs::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        for (unsigned line = 0; line < 105; ++line) {
            output << filler;
        }
        const auto tokens = 1000 + file;
        output << "{\"type\":\"assistant\",\"sessionId\":\"large-session\",\"timestamp\":\"2026-09-12T12:00:00Z\","
            "\"message\":{\"id\":\"large-" << file << "\",\"stop_reason\":\"end_turn\",\"usage\":{\"input_tokens\":"
            << tokens << "}}}\n";
        output.close();
        check(output.good(), "large transcript fixture written");
        history_bytes += fs::file_size(path);
        expected      += tokens;
    }
    check(history_bytes > 512ull * 1024 * 1024, "fixture exceeds the former whole-history budget");
    Scan_context context;
    const auto started  = std::chrono::steady_clock::now();
    const auto sessions = claude::scan_usage(profile, context);
    const auto elapsed  = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    std::cout << "scanned " << history_bytes << " history bytes in " << elapsed.count() << " ms\n";
    check(sessions.size() == 1 && sessions[0].record.total_tokens == expected && sessions[0].message_count == 5,
        "a history larger than the former budget scans completely");
    fs::remove_all(profile);
}

void live_transcript_contract(const fs::path& fixture)
{
    const auto profile = fixture / "live-profile";
    const auto live    = profile / "projects/active/live.jsonl";
    const auto steady  = profile / "projects/active/steady.jsonl";
    write(live,   assistant_record("live", "live-1", 10) + assistant_record("live", "live-2", 20));
    write(steady, assistant_record("steady", "steady-1", 100) + assistant_record("steady", "steady-2", 200));
    claude::Usage_cache_input cache;
    cache.storage_file = fixture / "owned-cache.jsonl";
    cache.incremental  = true;
    const auto session_total = [](const std::vector<claude::Usage_session>& sessions, std::string_view session) {
        std::uint64_t sum = 0;
        for (const auto& entry : sessions) {
            if (entry.record.session_key == session) {
                sum += entry.record.total_tokens;
            }
        }
        return sum;
    };

    // A transcript that grows while it is read keeps its previous evidence,
    // here none, instead of failing the scan of every other transcript.
    Live_writer_context writing(live);
    const auto racing = claude::scan_usage(profile, writing, cache);
    check(writing.appended() > 0, "the live writer appended during the scan");
    check(session_total(racing, "steady") == 300 && session_total(racing, "live") == 0,
        "a scan racing a live writer completes with the other transcripts");

    Scan_context quiet;
    const auto settled = claude::scan_usage(profile, quiet, cache);
    check(session_total(settled, "steady") == 300 &&
        session_total(settled, "live") == 30 + 7ull * writing.appended(),
        "the next scan reads the settled transcript completely");
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "expected built package directory\n";
        return 1;
    }
    int failures = 0;
    const auto run = [&failures](const char* name, const auto& contract) {
        try {
            Workspace workspace;
            contract(workspace.m_path);
        }
        catch (const std::exception& error) {
            std::cerr << name << ": " << error.what() << '\n';
            ++failures;
        }
    };
    const auto package = addon::path_from_utf8(argv[1]);
    run("adapter cache", [&package](const fs::path& fixture) { adapter_cache_contract(package, fixture); });
    run("large history", large_history_contract);
    run("live transcript", live_transcript_contract);
    return failures == 0 ? 0 : 1;
}
