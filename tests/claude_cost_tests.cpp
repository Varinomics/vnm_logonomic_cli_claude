#include "claude_accounts.h"
#include "claude_cost.h"
#include "claude_probes.h"

#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/json.h>
#include <logonomic_cli_addon/package_source.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

namespace addon = logonomic::cli_addon;
namespace fs    = std::filesystem;
using Json      = addon::Json_value;

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
    check((bool)output, "write isolated cost fixture");
}

class Temporary_directory
{
public:
    Temporary_directory()
    :
        m_path(fs::temp_directory_path() / ("claude-cost-" + addon::random_nonce_hex()))
    {
        check(fs::create_directory(m_path), "create isolated cost fixture");
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

class Context final : public addon::Invocation_context
{
public:
    const addon::Invoke_request& request() const override { return m_request; }
    bool cancel_requested() const override { return m_cancelled; }
    bool wait_for_cancel(int) override { return m_cancelled; }
    void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override {}
    void succeed(Json payload) override { m_payload = std::move(payload); m_finished = true; }
    void fail(addon::Result_status status, std::string, std::string) override
    {
        m_status   = status;
        m_finished = true;
    }
    bool finished() const override { return m_finished; }

    addon::Invoke_request m_request;
    Json                  m_payload;
    addon::Result_status  m_status = addon::Result_status::SUCCESS;
    bool                  m_finished = false;
    bool                  m_cancelled = false;
};

addon::Account_context account_for(const fs::path& profile)
{
    return {2, claude::account_key(profile), addon::path_to_utf8(profile)};
}

std::string native_cost(const std::string& session, double value)
{
    auto record = Json::object();
    record.set("type", Json("cost-state"));
    record.set("sessionId", Json(session));
    record.set("totalCostUSD", Json(value));
    return addon::serialize_json(record) + '\n';
}

Json receipt(const fs::path& profile, const std::string& session, double value)
{
    auto record = Json::object();
    record.set("version", Json(1));
    record.set("source", Json("claude-code-result"));
    record.set("billing", Json("api"));
    record.set("profile_path", Json(addon::path_to_utf8(profile)));
    record.set("session_id", Json(session));
    record.set("recorded_at_ms", Json((std::int64_t)1793577600000));
    record.set("total_cost_usd", Json(value));
    record.set("cost_basis", Json("list"));
    return record;
}

void write_receipt(const fs::path& profile, const std::string& session, double value)
{
    write(profile / "logonomic-cost-receipts" / (session + ".json"),
        addon::serialize_json(receipt(profile, session, value)));
}

std::optional<addon::Quota_cost> read(const fs::path& profile)
{
    Context context;
    return claude::read_recorded_cost(account_for(profile), context);
}

void expect_failure(const fs::path& profile, std::string_view reason)
{
    try {
        (void)read(profile);
    }
    catch (const std::runtime_error& error) {
        check(error.what() == reason, "the cost source reports the actual failure");
        return;
    }
    throw std::runtime_error("invalid cost evidence was accepted");
}

void native_snapshots(const fs::path& root)
{
    const auto profile = root / "native";
    const auto first   = profile / "projects/repository/first.jsonl";
    write(first, native_cost("first", 1.25) + native_cost("first", 2.5));
    auto message = Json::object();
    message.set("type", Json("assistant"));
    message.set("sessionId", Json("second"));
    message.set("message", Json(std::string(70 * 1024, 's')));
    write(profile / "projects/repository/second.jsonl",
        addon::serialize_json(message) + '\n' + native_cost("second", 4));
    auto value = read(profile);
    check(value && value->amount == 6.5 && value->estimated && value->currency == "USD" &&
        value->label == "Recorded sessions" && !value->available,
        "last native snapshots produce a scoped local estimate without a made-up balance");

    write(first, native_cost("first", 3.5) + "{\"type\":\"cost-state\"");
    value = read(profile);
    check(value && value->amount == 7.5, "a changed transcript refreshes while its torn final line is ignored");
    write(profile / "projects/repository/copy.jsonl", native_cost("first", 3.5));
    check(read(profile)->amount == 7.5, "a copied ancestor cost is not charged to its receiving file");
    write(profile / "projects/duplicate/first.jsonl", native_cost("first", 3.5));
    check(read(profile)->amount == 7.5, "identical copies of the same session are counted once");
    write(profile / "projects/duplicate/first.jsonl", native_cost("first", 3.75));
    expect_failure(profile, "cost_session_conflict");

    const auto fork = root / "fork";
    write(fork / "projects/repository/child.jsonl",
        "{\"type\":\"user\",\"sessionId\":\"child\",\"forkedFrom\":{\"sessionId\":\"parent\"}}\n" +
        native_cost("child", 6));
    expect_failure(fork, "cost_session_overlap");
}

void receipt_evidence(const fs::path& root)
{
    const auto profile = root / "receipts";
    fs::create_directories(profile);
    check(!read(profile), "a profile with no cost source is absent, not zero");
    write(profile / "stats-cache.json", "{\"modelUsage\":{\"sonnet\":{\"costUSD\":0}}}");
    check(!read(profile), "the stats cache's placeholder cost is not consumption evidence");
    write_receipt(profile, "review", 0);
    check(read(profile) && read(profile)->amount == 0, "an explicit zero receipt is a genuine recorded zero");
    write_receipt(profile, "review", 6.6474);
    write_receipt(profile, "other", 1.5);
    check(std::abs(read(profile)->amount - 8.1474) < 1e-10, "recorded receipts contribute once per session");
    write(profile / "projects/repository/review.jsonl", native_cost("review", 6.6474));
    check(std::abs(read(profile)->amount - 8.1474) < 1e-10,
        "a matching native snapshot and receipt are deduplicated");
    write(profile / "projects/repository/review.jsonl", native_cost("review", 8));
    expect_failure(profile, "cost_session_conflict");

    const auto wrong_profile = root / "wrong-profile";
    auto wrong = receipt(profile, "review", 5);
    write(wrong_profile / "logonomic-cost-receipts/review.json", addon::serialize_json(wrong));
    expect_failure(wrong_profile, "cost_receipt_profile_mismatch");

    const auto invalid = root / "invalid";
    write_receipt(invalid, "review", -1);
    expect_failure(invalid, "cost_amount_invalid");
    write(invalid / "logonomic-cost-receipts/review.json", "{\"total_cost_usd\":1e400}");
    expect_failure(invalid, "cost_record_invalid");

    const auto overlap = root / "receipt-with-native-overlap";
    write(overlap / "projects/repository/child.jsonl",
        "{\"type\":\"fork-context-ref\"}\n" + native_cost("child", 6));
    write_receipt(overlap, "review", 1.75);
    const auto reviews = read(overlap);
    check(reviews && reviews->amount == 1.75 && reviews->label == "Recorded API runs",
        "unusable native lineage leaves independently recorded reviews available with their narrower coverage");

    Context mismatched;
    auto wrong_account = account_for(profile);
    wrong_account.adapter_account_key = account_for(overlap).adapter_account_key;
    bool refused = false;
    try {
        (void)claude::read_recorded_cost(wrong_account, mismatched);
    }
    catch (const std::runtime_error& error) {
        refused = std::string_view(error.what()) == "account_profile_mismatch";
    }
    check(refused, "a source path cannot be borrowed from another selected account");

    Context cancelled;
    cancelled.m_cancelled = true;
    bool stopped = false;
    try {
        (void)claude::read_recorded_cost(account_for(profile), cancelled);
    }
    catch (const std::runtime_error& error) {
        stopped = std::string_view(error.what()) == "cancelled";
    }
    check(stopped, "cancelled cost reads do no further scanning");
}

addon::Probe_inspect_request inspect_request(const fs::path& profile, bool report_cost)
{
    addon::Probe_inspect_request request;
    request.account_context   = account_for(profile);
    request.report_quota_cost = report_cost;
    request.inspection.final  = true;
    request.inspection.stdout_bytes =
        R"({"type":"result","local_command":"usage","result":"Total cost: $0.0000"})";
    return request;
}

void packaged_dispatch(const fs::path& package, const fs::path& root)
{
    const auto profile = root / "packaged";
    write_receipt(profile, "review", 5.25);
    addon::Simulator_options options;
    options.package_directory    = package;
    options.profile_storage_root = root / "host-storage";
    addon::Host_simulator host(options, nullptr);
    addon::Contract_failure failure;
    check(host.start(&failure), "packaged adapter starts: " + failure.detail);
    const auto live = host.live_capabilities();
    check(std::find(live.begin(), live.end(), addon::Capability_pair{"quota-cost", 1}) != live.end(),
        "the built package negotiates quota-cost independently");
    const auto dispatch = [&host](const addon::Probe_inspect_request& request) {
        const auto id = host.invoke({"quota", 3}, "inspect", addon::encode_probe_inspect_request(request, 2), 10000);
        const auto outcome = host.wait_outcome(id, 11000);
        check(outcome && outcome->status == addon::Host_status::SUCCESS, "packaged quota inspection succeeds");
        const auto result = addon::decode_quota_inspect_result(outcome->payload, true, nullptr, 3);
        check(result.has_value(), "packaged inspection obeys the SDK grammar");
        return *result;
    };
    const auto hidden = dispatch(inspect_request(profile, false));
    check(!hidden.cost && !hidden.cost_failed, "costs leave the provider only after negotiation");
    const auto saved = dispatch(inspect_request(profile, true));
    check(saved.observation && saved.observation->state == addon::Quota_status::UNAVAILABLE &&
        saved.cost && saved.cost->amount == 5.25 && !saved.cost_failed,
        "saved cost survives unavailable quota without using the fresh probe's zero");

    auto failed_probe = inspect_request(profile, true);
    failed_probe.report_quota_failure = true;
    failed_probe.inspection.exit_code = 1;
    const auto failed = dispatch(failed_probe);
    check(failed.observation->state == addon::Quota_status::FAILED &&
        failed.failure_reason == addon::quota_failure_reason(addon::Quota_failure::PROCESS_FAILED) &&
        failed.diagnostic && failed.diagnostic->stage == addon::Quota_failure_stage::PROCESS_EXIT &&
        failed.diagnostic->code == 1 &&
        failed.cost && failed.cost->amount == 5.25 && !failed.cost_failed,
        "saved costs remain independent of the original failed quota probe and exit diagnostic");

    auto timed_out_probe = failed_probe;
    timed_out_probe.inspection.timed_out = true;
    const auto timed_out = dispatch(timed_out_probe);
    check(timed_out.observation->state == addon::Quota_status::FAILED &&
        timed_out.failure_reason == addon::quota_failure_reason(addon::Quota_failure::TIMED_OUT) &&
        timed_out.cost && timed_out.cost->amount == 5.25 && !timed_out.cost_failed,
        "saved costs do not replace a native quota timeout with a successful quota state");

    auto unbound_probe = failed_probe;
    unbound_probe.account_context.reset();
    const auto unbound = dispatch(unbound_probe);
    check(unbound.observation->state == addon::Quota_status::FAILED &&
        !unbound.cost && !unbound.cost_failed,
        "an unbound failed quota probe cannot borrow saved costs from an ambient profile");

    write_receipt(profile, "review", -2);
    const auto invalid = dispatch(inspect_request(profile, true));
    check(invalid.observation->state == addon::Quota_status::UNAVAILABLE &&
        !invalid.cost && invalid.cost_failed,
        "malformed optional cost evidence is a scoped failure, not missing evidence or zero");
    const auto invalid_failed = dispatch(failed_probe);
    check(invalid_failed.observation->state == addon::Quota_status::FAILED &&
        invalid_failed.failure_reason == addon::quota_failure_reason(addon::Quota_failure::PROCESS_FAILED) &&
        invalid_failed.diagnostic && invalid_failed.diagnostic->stage == addon::Quota_failure_stage::PROCESS_EXIT &&
        invalid_failed.diagnostic->code == 1 &&
        !invalid_failed.cost && invalid_failed.cost_failed,
        "a malformed receipt preserves both the native process failure and separate cost failure");

    auto windows = inspect_request(profile, true);
    windows.inspection.stdout_bytes =
        R"({"type":"result","local_command":"usage","result":"Session: 35% used"})";
    const auto subscription = dispatch(windows);
    check(subscription.observation->state == addon::Quota_status::READY &&
        subscription.observation->windows && subscription.observation->windows->size() == 1 &&
        subscription.observation->windows->front().used_percent == 35 &&
        !subscription.cost && !subscription.cost_failed,
        "healthy subscription windows use their own source without scanning unrelated saved costs");
    check(host.shutdown(1000, nullptr), "packaged adapter exits");
}

} // namespace

int main(int argc, char** argv)
{
    try {
        check(argc == 2, "cost tests require the built package directory");
        Temporary_directory fixture;
        native_snapshots(fixture.path());
        receipt_evidence(fixture.path());
        packaged_dispatch(addon::path_from_utf8(argv[1]), fixture.path());
        std::cout << "Claude recorded cost checks passed.\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
