#include "claude_accounts.h"
#include "claude_usage.h"

#include <logonomic_cli_addon_test/invocation_context.h>
#include <logonomic_cli_addon_test/usage_scan.h>

#include <logonomic_cli_addon/environment_bootstrap.h>
#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/profile_storage.h>
#include <logonomic_cli_addon/sha256.h>
#include <logonomic_cli_addon/usage_scan.h>
#include <logonomic_cli_addon/usage_incremental.h>

#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <utility>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace addon = logonomic::cli_addon;
namespace test = addon::test;
namespace fs = std::filesystem;

void check(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

using Cancelled_context = test::Cancelled_scan_context;

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

class Cancel_after_index_context final : public addon::Invocation_context
{
public:
    explicit Cancel_after_index_context(const addon::Usage_scan_request& input)
    :
        m_index(addon::path_from_utf8(input.cache_selection->candidate_root) / "index.jsonl")
    {
        m_request.capability = {"usage-incremental", 1};
        m_request.operation = "scan";
        m_request.payload = addon::encode_usage_incremental_request(input);
    }

    const addon::Invoke_request& request() const override { return m_request; }
    bool cancel_requested() const override { return fs::exists(m_index); }
    bool wait_for_cancel(int) override { return cancel_requested(); }
    void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override {}
    void succeed(addon::Json_value) override { throw std::runtime_error("cancelled incremental scan succeeded"); }
    void fail(addon::Result_status status, std::string reason, std::string) override
    {
        m_cancelled = status == addon::Result_status::CANCELLED && reason == "cancelled";
    }
    bool finished() const override { return m_cancelled; }

private:
    addon::Invoke_request m_request;
    fs::path m_index;
    bool m_cancelled = false;
};

class Failing_progress_context final : public addon::Invocation_context
{
public:
    Failing_progress_context(const addon::Usage_scan_request& input, std::exception_ptr failure)
    :
        m_failure(std::move(failure))
    {
        m_request.capability = {"usage-scan", 3};
        m_request.operation = "scan";
        m_request.payload = addon::encode_usage_scan_request(input, 3);
    }

    const addon::Invoke_request& request() const override { return m_request; }
    bool cancel_requested() const override { return false; }
    bool wait_for_cancel(int) override { return false; }
    void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override
    {
        std::rethrow_exception(m_failure);
    }
    void succeed(addon::Json_value) override { throw std::runtime_error("failure fixture succeeded"); }
    void fail(addon::Result_status status, std::string reason, std::string diagnostic) override
    {
        m_status = status;
        m_reason = std::move(reason);
        m_diagnostic = std::move(diagnostic);
        m_finished = true;
    }
    bool finished() const override { return m_finished; }

    addon::Result_status m_status = addon::Result_status::SUCCESS;
    std::string m_reason;
    std::string m_diagnostic;

private:
    addon::Invoke_request m_request;
    std::exception_ptr m_failure;
    bool m_finished = false;
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

// The host cancels the scan when it reports its first progress.
class Cancel_on_progress_context final : public addon::Invocation_context
{
public:
    const addon::Invoke_request& request() const override { return m_request; }
    bool cancel_requested() const override { return m_progress_reports > 0; }
    bool wait_for_cancel(int) override { return cancel_requested(); }
    void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override
    {
        ++m_progress_reports;
    }
    void succeed(addon::Json_value) override {}
    void fail(addon::Result_status, std::string, std::string) override {}
    bool finished() const override { return false; }

    unsigned progress_reports() const { return m_progress_reports; }

private:
    addon::Invoke_request m_request;
    unsigned              m_progress_reports = 0;
};

// The host cancels the scan once the scan has written its whole new cache to
// a temporary file, before that file replaces the cache.
class Cancel_on_written_cache_context final : public addon::Invocation_context
{
public:
    Cancel_on_written_cache_context(fs::path directory, std::uintmax_t cache_size)
    :
        m_directory(std::move(directory)),
        m_cache_size(cache_size)
    {}

    const addon::Invoke_request& request() const override { return m_request; }
    bool cancel_requested() const override
    {
        for (const auto& entry : fs::directory_iterator(m_directory)) {
            const auto name = entry.path().filename().string();
            std::error_code error;
            if (name.starts_with("claude-cache-") && name.ends_with(".tmp") &&
                fs::file_size(entry.path(), error) == m_cache_size && !error)
            {
                return true;
            }
        }
        return false;
    }
    bool wait_for_cancel(int) override { return cancel_requested(); }
    void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override {}
    void succeed(addon::Json_value) override {}
    void fail(addon::Result_status, std::string, std::string) override {}
    bool finished() const override { return false; }

private:
    addon::Invoke_request m_request;
    fs::path              m_directory;
    std::uintmax_t        m_cache_size;
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
        fs::remove_all(addon::usage_cache_io_path(m_path), error);
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

// Rewrites a cache in the format that kept each transcript on one line.
void write_single_line_cache(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    std::string line;
    std::string output;
    std::optional<addon::Json_value> pending;
    const auto flush = [&] {
        if (pending) output += addon::serialize_json(*pending) + '\n';
        pending.reset();
    };
    addon::Json_parse_limits limits;
    limits.max_bytes = limits.max_string_bytes = 64 * 1024 * 1024;
    while (std::getline(input, line)) {
        auto value = addon::parse_json(line, limits, nullptr);
        check(value && value->is_object(), "cache line parses");
        if (const auto* format = value->find("format")) {
            check(format->is_string(), "cache header names its format");
            value->set("format", addon::Json_value("claude-message-cache-v1"));
            output += addon::serialize_json(*value) + '\n';
            continue;
        }
        if (pending && *pending->find("path") == *value->find("path")) {
            for (const auto& record : value->find("records")->as_array()) {
                pending->find("records")->push_back(record);
            }
            continue;
        }
        flush();
        pending = std::move(value);
    }
    flush();
    input.close();
    write(path, output);
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

    check(test::usage_token_total(test::usage_scan_pages(host, request, 2)) == 2700,
        "v2 retains its existing namespace and telemetry");
    for (const auto& path : sources) {
        check(fs::remove(path), "remove fixture source");
    }
    select("empty-selection", std::nullopt);
    const auto empty = test::usage_scan_pages(host, request, 3);
    check(empty.records.empty() && empty.cache_candidate,
        "absent host selection never bootstraps from mutable provider cache");
    validate(*empty.cache_candidate);

    sources = write_sources(profile, 10);
    select("accepted", std::nullopt);
    const auto first_request = request;
    const auto first = test::usage_scan_page(host, request, 3);
    check(!first.complete && first.records.size() == 256 && first.cache_candidate,
        "packaged first page supplies a sealed bounded candidate");
    validate(*first.cache_candidate);
    request.snapshot_id = first.snapshot_id;
    request.cursor = first.next_cursor;
    auto changed = request;
    changed.mode = "repair";
    check(test::usage_scan_outcome(host, changed, 3).status != addon::Host_status::SUCCESS,
        "continuation cannot change the frozen request");
    const auto last = test::usage_scan_page(host, request, 3);
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
    const auto version_one = test::usage_scan_outcome(host, request, 3);
    check(version_one.status != addon::Host_status::SUCCESS &&
        version_one.reason_code == "account_unavailable" &&
        version_one.message.find("selected account has no available profile") != std::string::npos,
        "version-one account without a profile locator retains its explicit refusal");
    request.context = first_request.context;
    validate(*first.cache_candidate);

    select("account-absent", std::nullopt);
    request.context.account_context.reset();
    request.context.launch_environment->effective_environment.set("CLAUDE_CONFIG_DIR", addon::Json_value(native_profile));
    const auto account_absent = test::usage_scan_pages(host, request, 3);
    check(account_absent.records.size() == 270 && test::usage_token_total(account_absent) == 2700,
        "environment-selected profile keeps account context absent on every page");
    request.context = first_request.context;

    sources = write_sources(profile, 1);
    select("unaccepted", first.cache_candidate);
    request.mode = "repair";
    const auto lower = test::usage_scan_pages(host, request, 3);
    check(test::usage_token_total(lower) == 270 && lower.cache_candidate,
        "lower native observation is a separate candidate");
    validate(*first.cache_candidate);
    for (const auto& path : sources) {
        check(fs::remove(path), "remove refreshed fixture source");
    }

    // The host rejects the lower partial aggregate and continues selecting the coherent original cache.
    select("recovered", first.cache_candidate);
    const auto recovered = test::usage_scan_pages(host, request, 3);
    check(test::usage_token_total(recovered) == 2700,
        "rejected candidate cannot become retained history after source deletion");
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
    const auto reset = test::usage_scan_pages(host, request, 3);
    check(reset.records.empty() && reset.cache_candidate, "reset reconstructs only current sources");
    validate(*reset.cache_candidate);
    validate(*first.cache_candidate);
    auto invalid_reset = request;
    invalid_reset.cache_selection->accepted = first.cache_candidate;
    check(test::usage_scan_outcome(host, invalid_reset, 3).status != addon::Host_status::SUCCESS,
        "reset cannot import a retained selection");

    auto old_request = first_request;
    old_request.cache_selection.reset();
    check(test::usage_token_total(test::usage_scan_pages(host, old_request, 2)) == 2700,
        "v3 scans and reset leave v2 cache untouched");

    select("occupied", first.cache_candidate);
    write(addon::usage_cache_io_path(addon::path_from_utf8(request.cache_selection->candidate_root) / "foreign"),
        "occupied");
    const auto occupied = test::usage_scan_outcome(host, request, 3);
    check(occupied.status != addon::Host_status::SUCCESS &&
        occupied.reason_code == "usage_cache_unavailable" &&
        occupied.message.find("checking usage cache storage") != std::string::npos,
        "provider refuses a nonempty host allocation");
    validate(*first.cache_candidate);

    const auto mutable_path = storage / "claude-message-cache-v1.jsonl";
    const auto conflicting = mutable_path / "host-candidate";
    if (!fs::exists(mutable_path) || fs::is_directory(mutable_path)) {
        fs::create_directories(conflicting);
    }
    request.cache_selection = addon::Usage_cache_selection{std::nullopt, addon::path_to_utf8(conflicting)};
    check(test::usage_scan_outcome(host, request, 3).status != addon::Host_status::SUCCESS,
        "host candidates cannot enter the provider mutable namespace");

    select("wrong-account", first.cache_candidate);
    auto wrong_account = request;
    const auto other_profile = fixture / "other-native-profile";
    fs::create_directories(other_profile);
    wrong_account.context.account_context = addon::Account_context{
        2, claude::account_key(other_profile), addon::path_to_utf8(other_profile)};
    wrong_account.context.launch_environment->bootstrap =
        addon::Environment_bootstrap_evidence{addon::path_to_utf8(other_profile)};
    check(test::usage_scan_outcome(host, wrong_account, 3).status != addon::Host_status::SUCCESS,
        "selected cache cannot cross native profile and account ownership");
    validate(*first.cache_candidate);

    auto temporary_conflict = storage / "claude-cache-host.tmp";
    while (temporary_conflict.native().size() < 291) {
        temporary_conflict /= "retained-account-cache-allocation-component";
    }
    check(fs::create_directories(addon::usage_cache_io_path(temporary_conflict)),
        "allocate long descendant of the root-owned mutable temporary namespace");
    request.cache_selection = addon::Usage_cache_selection{std::nullopt, addon::path_to_utf8(temporary_conflict)};
    check(test::usage_scan_outcome(host, request, 3).status != addon::Host_status::SUCCESS,
        "host allocation cannot alias the mutable temporary-file namespace");

    select("corrupt-input", first.cache_candidate);
    write(addon::usage_cache_io_path(addon::path_from_utf8(first.cache_candidate->root) / "unlisted"), "unexpected");
    check(test::usage_scan_outcome(host, request, 3).status != addon::Host_status::SUCCESS,
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
    const auto base = test::usage_scan_pages(host, request, 3);
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
    const auto next = test::usage_scan_pages(host, request, 3);
    check(session_total(next, "steady") == 600, "an unchanged transcript is not parsed again");
    check(session_total(next, "growing") == 100,
        "an appended transcript adds exactly its new records, once each");
    validate(*next.cache_candidate);

    // A candidate in the format that kept each transcript on one line
    // upgrades with the same totals.
    const auto single_line_root = addon::path_from_utf8(next.cache_candidate->root).parent_path() / "single-line-format";
    check(fs::create_directories(addon::usage_cache_io_path(single_line_root)), "allocate single-line fixture");
    const auto single_line_file = addon::usage_cache_io_path(single_line_root) / "messages.jsonl";
    check(fs::copy_file(addon::usage_cache_io_path(addon::path_from_utf8(next.cache_candidate->root)) / "messages.jsonl",
        single_line_file), "copy accepted messages into the fixture");
    write_single_line_cache(single_line_file);
    const auto single_line = addon::seal_usage_cache_candidate(addon::path_to_utf8(single_line_root), *allocated,
        request.context.account_context, next.cache_candidate->snapshot_id, "claude-message-cache@1",
        [] { return false; }, &failure);
    check(single_line.has_value(), "seal single-line fixture");
    select("single-line-upgrade", single_line);
    const auto upgraded = test::usage_scan_pages(host, request, 3);
    check(session_total(upgraded, "steady") == 600 && session_total(upgraded, "growing") == 100,
        "a single-line candidate upgrades with exact totals");
    validate(*upgraded.cache_candidate);

    select("incremental-repair", next.cache_candidate);
    request.mode = "repair";
    const auto invalid_json = test::usage_scan_outcome(host, request, 3);
    check(invalid_json.status != addon::Host_status::SUCCESS &&
        invalid_json.reason_code == "usage_json_invalid" &&
        invalid_json.message.find("not valid JSON") != std::string::npos,
        "repair rereads every transcript instead of trusting its recorded fingerprint");

    write(steady, message("steady", "steady-1", 600));
    const std::string private_text = "credential-and-transcript-content-must-stay-private";
    const auto invalid_counter = profile / "projects" / private_text / "message.jsonl";
    auto invalid_record = message("private-session", "private-message", 10);
    const auto token_value = invalid_record.find("\"input_tokens\":10") + std::string_view("\"input_tokens\":").size();
    invalid_record.replace(token_value, 2, '"' + private_text + '"');
    write(invalid_counter, invalid_record);
    select("invalid-counter", next.cache_candidate);
    request.mode = "repair";
    const auto counter_failure = test::usage_scan_outcome(host, request, 3);
    check(counter_failure.status != addon::Host_status::SUCCESS &&
        counter_failure.reason_code == "usage_counter_invalid" &&
        counter_failure.message.find("invalid token count") != std::string::npos,
        "actual adapter explains the failing usage check");
    check(counter_failure.message.find(private_text) == std::string::npos &&
        counter_failure.message.find(native_profile) == std::string::npos &&
        host.stderr_text().find(private_text) == std::string::npos,
        "actual adapter does not expose transcript content or native profile paths");
    validate(*next.cache_candidate);
    check(fs::remove(invalid_counter), "remove invalid counter fixture");

    const auto diagnostic_for = [&](std::string name, std::exception_ptr exception) {
        select(std::move(name), next.cache_candidate);
        Failing_progress_context context(request, std::move(exception));
        claude::handle_usage(context);
        check(context.finished() && context.m_status == addon::Result_status::FAILED,
            "usage boundary reports the injected failure");
        check(context.m_diagnostic.find(private_text) == std::string::npos &&
            context.m_diagnostic.find(native_profile) == std::string::npos,
            "exception messages and filesystem paths stay private");
        validate(*next.cache_candidate);
        return std::pair{context.m_reason, context.m_diagnostic};
    };
    const auto unexpected = diagnostic_for("unexpected-exception",
        std::make_exception_ptr(std::runtime_error(private_text)));
    check(unexpected.first == "claude_usage_failed" &&
        unexpected.second.find("unexpected provider error") != std::string::npos,
        "unknown exception text is replaced with a static explanation");
    const auto filesystem = diagnostic_for("filesystem-exception", std::make_exception_ptr(
        fs::filesystem_error(private_text, invalid_counter, std::make_error_code(std::errc::permission_denied))));
    check(filesystem.first == "claude_usage_filesystem_failed" &&
        filesystem.second.find("system error " +
            std::to_string(std::make_error_code(std::errc::permission_denied).value())) != std::string::npos,
        "filesystem failure retains its numeric cause without private exception text");
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

void many_messages_contract(const fs::path& fixture)
{
    // Nor is the number of messages bounded: one transcript holds more than a
    // million completed messages.
    const auto profile = fixture / "many-profile";
    const auto path    = profile / "projects/many/session.jsonl";
    constexpr unsigned messages = 1000001;
    fs::create_directories(path.parent_path());
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        for (unsigned index = 0; index < messages; ++index) {
            output << assistant_record("many", "message-" + std::to_string(index), 1);
        }
        output.close();
        check(output.good(), "many-message transcript written");
    }
    Scan_context context;
    const auto started  = std::chrono::steady_clock::now();
    const auto sessions = claude::scan_usage(profile, context);
    const auto elapsed  = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    std::cout << "scanned " << messages << " messages in " << elapsed.count() << " ms\n";
    check(sessions.size() == 1 && sessions[0].message_count == messages &&
        sessions[0].record.total_tokens == messages,
        "a history of more than a million messages scans completely");
    fs::remove_all(profile);
}

void incremental_cache_contract(const fs::path& package, const fs::path& fixture)
{
    const auto profile = fixture / "native-profile";
    const auto source = profile / "projects/project/a.jsonl";
    const auto other = profile / "projects/project/b.jsonl";
    const auto copy = profile / "projects/project/z.jsonl";
    const auto repository = fixture / "repository";
    fs::create_directories(repository);
    const auto record = [&](std::string_view session, std::string_view id, unsigned tokens) {
        auto value = addon::parse_json(assistant_record(session, id, tokens), {}, nullptr);
        check(value.has_value(), "incremental fixture record parses");
        value->set("cwd", addon::Json_value(addon::path_to_utf8(repository)));
        return addon::serialize_json(*value) + '\n';
    };
    write(source, record("main", "shared", 10) + record("main", "private", 2));
    write(other, record("other", "other-message", 7));
    write(copy, record("copy", "shared", 10));

    addon::Simulator_options options;
    options.package_directory = package;
    options.profile_storage_root = fixture / "host-storage";
    addon::Host_simulator host(options, nullptr);
    addon::Contract_failure failure;
    check(host.start(&failure), failure.detail);
    const auto account = claude::account_key(profile);
    std::string error;
    const auto allocated = addon::allocate_profile_storage_root(
        host.profile_storage_root(), "logonomic.cli.claude", &error, account);
    check(allocated.has_value(), error);
    addon::Usage_scan_request request;
    request.context.launcher = {"claude", addon::k_runner_direct, {}};
    request.context.launcher_argv = {"claude"};
    request.context.profile_storage = addon::Profile_storage_context{1, *allocated};
    request.context.account_context = addon::Account_context{2, account, addon::path_to_utf8(profile)};
    request.context.launch_environment = addon::Launch_environment{
        addon::host_platform_id(), addon::Json_value::object(),
        addon::Environment_bootstrap_evidence{addon::path_to_utf8(profile)}};
    addon::Simulator_invocation_options invocation;
    invocation.profile_storage = request.context.profile_storage;
    const auto acknowledge = host.invoke(
        {"profile-storage", 1}, "acknowledge", addon::Json_value::object(), 5000, invocation);
    const auto acknowledged = host.wait_outcome(acknowledge, 6000);
    check(acknowledged && acknowledged->status == addon::Host_status::SUCCESS, "incremental storage acknowledged");
    unsigned generation = 0;
    const auto select = [&](std::optional<addon::Usage_cache_reference> accepted) {
        request.snapshot_id.reset();
        request.cursor.reset();
        const auto root = addon::path_from_utf8(*allocated) / ("generation-" + std::to_string(++generation));
        fs::create_directories(root);
        request.cache_selection = addon::Usage_cache_selection{std::move(accepted), addon::path_to_utf8(root)};
    };
    const auto invoke = [&](bool incremental) {
        const auto id = host.invoke({incremental ? "usage-incremental" : "usage-scan", (std::uint16_t)(incremental ? 1 : 3)},
            "scan", incremental ? addon::encode_usage_incremental_request(request) :
                addon::encode_usage_scan_request(request, 3), 30000);
        const auto outcome = host.wait_outcome(id, 31000);
        check(outcome.has_value(), "incremental scan finishes");
        return *outcome;
    };
    const auto scan = [&] {
        const auto outcome = invoke(true);
        check(outcome.status == addon::Host_status::SUCCESS, "incremental scan succeeds: " + outcome.reason_code);
        const auto result = addon::decode_usage_incremental_result(outcome.payload, &failure);
        check(result.has_value(), failure.detail);
        check(result->baseline == *request.cache_selection->accepted, "update binds the exact accepted generation");
        return *result;
    };
    const auto full = [&] {
        const auto outcome = invoke(false);
        check(outcome.status == addon::Host_status::SUCCESS, "complete scan succeeds: " + outcome.reason_code);
        const auto page = addon::decode_usage_scan_result(outcome.payload, &failure, 3);
        check(page.has_value(), failure.detail);
        return *page;
    };
    const auto validate = [&](const addon::Usage_cache_reference& reference) {
        check(addon::validate_usage_cache_candidate(reference, *allocated, request.context.account_context,
            [] { return false; }, &failure).has_value(), "incremental candidate is self-contained: " + failure.detail);
    };

    select(std::nullopt);
    const auto first = full();
    check(first.records.size() == 2 && first.records[0].total_tokens == 12 && first.records[1].total_tokens == 7,
        "complete snapshot deduplicates the retained fork");
    select(first.cache_candidate);
    const auto adopted = scan();
    check(adopted.changes && adopted.changes->complete && adopted.changes->records.size() == 2,
        "monolithic conversion publishes saved observations because it lacks prior repository allocations");
    auto accepted = *adopted.changes->cache_candidate;
    validate(accepted);
    select(accepted);
    const auto unchanged = scan();
    check(!unchanged.changes && fs::is_empty(addon::path_from_utf8(request.cache_selection->candidate_root)),
        "unchanged scan preserves its accepted generation without writing or exporting accounting");

    write(profile / "projects/project/0.jsonl", record("earlier-copy", "shared", 10));
    select(accepted);
    const auto earlier = scan();
    check(earlier.changes && earlier.changes->records.empty(),
        "an earlier-sorted duplicate keeps its accepted owner and cannot add the same usage to another session");
    accepted = *earlier.changes->cache_candidate;
    select(accepted);
    const auto ownership_replay = full();
    check(ownership_replay.records.size() == 2 && ownership_replay.records[0].record_id == "main" &&
        ownership_replay.records[0].total_tokens == 12 && ownership_replay.records[1].total_tokens == 7,
        "complete replay retains the original owner together with its other messages");
    select(ownership_replay.cache_candidate);
    const auto readopted = scan();
    check(readopted.changes && readopted.changes->records.size() == 2 &&
        readopted.changes->records[0].record_id == "main" && readopted.changes->records[0].total_tokens == 12,
        "ownership survives a complete-snapshot cache round trip and subsequent adoption");
    accepted = *readopted.changes->cache_candidate;
    validate(accepted);

    const auto native_seed = fixture / "incoming-seed.jsonl";
    addon::Json_value native_message = addon::Json_value::object();
    native_message.set("record_id", addon::Json_value("unrelated-seed-message"));
    native_message.set("session_id", addon::Json_value("seed-session"));
    native_message.set("completed_date", addon::Json_value("2026-09-12"));
    native_message.set("source_path", addon::Json_value(addon::path_to_utf8(other)));
    native_message.set("total_tokens", addon::Json_value(9));
    addon::Json_value seed_records = addon::Json_value::array();
    seed_records.push_back(std::move(native_message));
    addon::Json_value seed_wrapper = addon::Json_value::object();
    seed_wrapper.set("path", addon::Json_value(addon::path_to_utf8(other)));
    seed_wrapper.set("records", std::move(seed_records));
    const auto seed_bytes = addon::serialize_json(seed_wrapper) + '\n';
    write(native_seed, seed_bytes);
    select(accepted);
    request.native_file_cache = addon::Usage_native_file_cache{
        addon::path_to_utf8(native_seed), addon::to_hex(addon::Sha256::digest(seed_bytes)), account, true};
    const auto seed_mismatch = invoke(false);
    check(seed_mismatch.status == addon::Host_status::FAILED && seed_mismatch.reason_code == "usage_seed_receipt_mismatch",
        "partitioned accepted cache applies the same nonempty native-seed receipt check as a monolithic cache");
    request.native_file_cache.reset();
    validate(accepted);

    write(source, record("main", "shared", 10) + record("main", "private", 2) + record("main", "appended", 3));
    select(accepted);
    auto changed = scan();
    check(changed.changes && changed.changes->records.size() == 1 &&
        changed.changes->records.front().record_id == "main" && changed.changes->records.front().total_tokens == 15,
        "append exports the affected coherent session only");
    accepted = *changed.changes->cache_candidate;
    validate(accepted);
    fs::remove_all(addon::usage_cache_io_path(addon::path_from_utf8(first.cache_candidate->root)));
    fs::remove_all(addon::usage_cache_io_path(addon::path_from_utf8(adopted.changes->cache_candidate->root)));
    validate(accepted);
    fs::remove(other);
    select(accepted);
    check(!scan().changes, "disappeared transcript retains its accepted observations and source evidence");

    const auto old_time = fs::last_write_time(source);
    write(source, record("main", "shared", 10) + record("main", "private", 4) + record("main", "appended", 3));
    fs::last_write_time(source, old_time + std::chrono::seconds(1));
    select(accepted);
    changed = scan();
    check(changed.changes && changed.changes->records.size() == 1 && changed.changes->records[0].total_tokens == 17,
        "same-size old edit reuses retained duplicate evidence and corrects its complete session");
    accepted = *changed.changes->cache_candidate;
    validate(accepted);

    fs::create_directories(repository / ".git");
    select(accepted);
    changed = scan();
    check(changed.changes && changed.changes->records.size() == 2 &&
        changed.changes->records[0].repository_usage->allocations.size() == 1 &&
        changed.changes->records[1].repository_usage->allocations.size() == 1,
        "repository boundary changes update attribution for live and retained sessions");
    accepted = *changed.changes->cache_candidate;
    validate(accepted);
    select(accepted);
    const auto replay = full();
    check(replay.records.size() == 2 && replay.records[0].total_tokens == 17 && replay.records[1].total_tokens == 7,
        "usage-scan@3 replays the partitioned cache as a complete snapshot including removed sources");

    const auto retained_source = record("main", "shared", 10) + record("main", "private", 4) +
        record("main", "appended", 3);
    write(source, retained_source + record("main", "cancelled", 5));
    select(accepted);
    Cancel_after_index_context cancelled(request);
    claude::handle_usage(cancelled);
    check(cancelled.finished() && !fs::exists(addon::path_from_utf8(request.cache_selection->candidate_root) /
        "cache-manifest.json"), "cancellation before sealing never publishes a partial generation");
    validate(accepted);
    write(source, retained_source);

    write(copy, record("copy", "shared", 11));
    select(accepted);
    check(invoke(true).status == addon::Host_status::FAILED,
        "conflicting changed duplicate is checked against the original retained copy");
    validate(accepted);
    write(copy, record("copy", "shared", 10));
    select(accepted);
    const auto metadata_only = scan();
    check(metadata_only.changes && metadata_only.changes->records.empty(),
        "unchanged accounting with a new source fingerprint publishes metadata only");
    accepted = *metadata_only.changes->cache_candidate;
    validate(accepted);

    auto changed_time = record("copy", "shared", 10);
    const auto noon = changed_time.find("T12:00:00Z");
    check(noon != std::string::npos, "temporal fixture instant exists");
    changed_time.replace(noon, 10, "T13:00:00Z");
    write(copy, changed_time);
    select(accepted);
    const auto temporal = scan();
    check(temporal.changes && temporal.changes->records.size() == 1 &&
        temporal.changes->records[0].record_id == "main" && temporal.changes->records[0].total_tokens == 17 &&
        temporal.changes->records[0].temporal && temporal.changes->records[0].temporal->unavailable_tokens == 10,
        "changed duplicate temporal evidence updates its owning session even when totals are unchanged");
    accepted = *temporal.changes->cache_candidate;
    validate(accepted);

    select(accepted);
    const auto index_path = addon::path_from_utf8(accepted.root) / "index.jsonl";
    std::ofstream tamper(index_path, std::ios::binary | std::ios::app);
    tamper << " ";
    tamper.close();
    check(invoke(true).status == addon::Host_status::FAILED,
        "metadata tampering cannot produce an unchanged acknowledgment");
}

void bounded_cache_lines_contract(const fs::path& fixture)
{
    // A transcript's cached messages are written in bounded lines, so the
    // cache stays readable however many messages one transcript holds.
    const auto profile    = fixture / "chunked-profile";
    const auto transcript = profile / "projects/chunked/session.jsonl";
    constexpr unsigned messages = 20000;
    std::string bytes;
    for (unsigned index = 0; index < messages; ++index) {
        bytes += assistant_record("chunked", "message-" + std::to_string(index), 3);
    }
    write(transcript, bytes);
    claude::Usage_cache_input cache;
    cache.storage_file = fixture / "chunked-cache.jsonl";
    cache.incremental  = true;
    Scan_context context;
    const auto expect_exact = [&](const char* message) {
        const auto sessions = claude::scan_usage(profile, context, cache);
        check(sessions.size() == 1 && sessions[0].message_count == messages &&
            sessions[0].record.total_tokens == 3ull * messages, message);
    };
    const auto cache_lines = [&] {
        std::ifstream input(*cache.storage_file, std::ios::binary);
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(input, line)) {
            check(line.size() <= 2 * 1024 * 1024, "every cache line stays far below the record bound");
            lines.push_back(std::move(line));
        }
        return lines;
    };
    expect_exact("the first scan counts every message");
    check(cache_lines().size() > 2, "one long transcript spans several cache lines");
    fs::remove(transcript);
    expect_exact("the persisted cache alone keeps the exact totals");

    write_single_line_cache(*cache.storage_file);
    expect_exact("a single-line cache upgrades with exact totals");
    const auto upgraded = cache_lines();
    check(upgraded.size() > 2 && upgraded.front().find("claude-message-cache-v2") != std::string::npos,
        "the upgraded cache is rewritten in bounded lines");
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

void cancelled_scan_contract(const fs::path& fixture)
{
    // A cancelled scan stops at its next transcript and keeps the cache it
    // started from, so the next scan still has the exact totals.
    const auto profile = fixture / "cancelled-profile";
    for (unsigned index = 0; index < 3; ++index) {
        const auto session = "cancelled-" + std::to_string(index);
        const auto path    = profile / "projects/cancelled" / (session + ".jsonl");
        write(path, assistant_record(session, session + "-1", 5));
    }
    claude::Usage_cache_input cache;
    cache.storage_file = fixture / "cancelled-cache.jsonl";
    cache.incremental  = true;
    const auto cache_bytes = [&] {
        std::ifstream input(*cache.storage_file, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    };
    const auto total = [](const std::vector<claude::Usage_session>& sessions) {
        std::uint64_t sum = 0;
        for (const auto& session : sessions) {
            sum += session.record.total_tokens;
        }
        return sum;
    };
    const auto failure_of = [&](addon::Invocation_context& context) {
        try {
            claude::scan_usage(profile, context, cache);
        }
        catch (const std::runtime_error& error) {
            return std::string(error.what());
        }
        return std::string{};
    };
    Scan_context quiet;
    check(total(claude::scan_usage(profile, quiet, cache)) == 15, "the first scan counts every transcript");
    const auto retained = cache_bytes();

    Cancel_on_progress_context cancelling;
    check(failure_of(cancelling) == "cancelled", "a cancelled scan reports cancellation");
    check(cancelling.progress_reports() == 1, "a cancelled scan stops before its next transcript");
    check(cache_bytes() == retained, "a cancelled scan leaves the retained cache unchanged");
    check(total(claude::scan_usage(profile, quiet, cache)) == 15,
        "the scan after a cancelled one keeps the exact totals");

    // Replacing the cache commits the scan: cancellation that arrives once the
    // new cache is written, but before it replaces the retained one, keeps the
    // retained cache. A changed transcript makes the new cache differ from it.
    const auto changed = assistant_record("cancelled-0", "cancelled-0-1", 5) +
        assistant_record("cancelled-0", "cancelled-0-2", 5);
    write(profile / "projects/cancelled/cancelled-0.jsonl", changed);
    auto reference = cache;
    reference.storage_file = fixture / "reference/cancelled-cache.jsonl";
    fs::create_directories(reference.storage_file->parent_path());
    fs::copy_file(*cache.storage_file, *reference.storage_file);
    check(total(claude::scan_usage(profile, quiet, reference)) == 20, "the changed transcript adds its message");
    check(fs::file_size(*reference.storage_file) != retained.size(), "the new cache differs from the retained one");

    Cancel_on_written_cache_context committing(fixture, fs::file_size(*reference.storage_file));
    check(failure_of(committing) == "cancelled",
        "a scan cancelled before it replaces the cache reports cancellation");
    check(cache_bytes() == retained, "a scan cancelled before it replaces the cache keeps the retained cache");
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
    run("incremental cache", [&package](const fs::path& fixture) { incremental_cache_contract(package, fixture); });
    run("large history", large_history_contract);
    run("many messages", many_messages_contract);
    run("bounded cache lines", bounded_cache_lines_contract);
    run("live transcript", live_transcript_contract);
    run("cancelled scan", cancelled_scan_contract);
    return failures == 0 ? 0 : 1;
}
