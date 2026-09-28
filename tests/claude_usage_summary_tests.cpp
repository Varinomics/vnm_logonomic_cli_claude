// Claude Code's saved usage, read from the selected profile's
// stats-cache.json through usage-lifetime-summary@1 and
// usage-saved-daily-tokens@1: valid, missing, malformed and cross-profile
// caches read in process, then the same capabilities through the packaged
// adapter as the host dispatches them.

#include "claude_accounts.h"
#include "claude_usage_summary.h"

#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/sha256.h>
#include <logonomic_cli_addon/usage_lifetime_summary.h>
#include <logonomic_cli_addon/usage_saved_daily_tokens.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
    check(static_cast<bool>(output), "write fixture file " + addon::path_to_utf8(path));
}

class Temporary_directory
{
public:
    Temporary_directory()
    :
        m_path(fs::temp_directory_path() / ("claude-usage-summary-" + addon::random_nonce_hex()))
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

class Context final : public addon::Invocation_context
{
public:
    Context(std::string capability, Json payload)
    {
        m_request.capability = {std::move(capability), 1};
        m_request.operation  = "read";
        m_request.payload    = std::move(payload);
    }

    const addon::Invoke_request& request() const override { return m_request; }
    bool cancel_requested() const override { return false; }
    bool wait_for_cancel(int) override { return false; }
    void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override {}
    void succeed(Json payload) override
    {
        m_payload  = std::move(payload);
        m_finished = true;
    }
    void fail(addon::Result_status status, std::string reason, std::string) override
    {
        m_status   = status;
        m_reason   = std::move(reason);
        m_finished = true;
    }
    bool finished() const override { return m_finished; }

    addon::Invoke_request m_request;
    Json                  m_payload;
    addon::Result_status  m_status = addon::Result_status::SUCCESS;
    std::string           m_reason;
    bool                  m_finished = false;
};

// What one in-process read answered. A thrown error is what the adapter
// reports as FAILED with claude_operation_failed.
struct Answer
{
    bool                 threw  = false;
    addon::Result_status status = addon::Result_status::SUCCESS;
    std::string          reason;
    Json                 payload;
};

addon::Account_context account_for(const fs::path& profile)
{
    return addon::Account_context{2, claude::account_key(profile), addon::path_to_utf8(profile)};
}

Answer read_saved(std::string_view capability, const addon::Account_context& account)
{
    const bool lifetime = capability == "usage-lifetime-summary";
    Context context(std::string(capability), lifetime
        ? addon::encode_usage_lifetime_summary_request({account})
        : addon::encode_usage_saved_daily_tokens_request({account}));
    Answer answer;
    try {
        if (lifetime) {
            claude::handle_usage_summary(context);
        }
        else {
            claude::handle_usage_daily(context);
        }
    }
    catch (const std::exception&) {
        answer.threw = true;
        return answer;
    }
    check(context.m_finished, std::string(capability) + " answers every read");
    answer.status  = context.m_status;
    answer.reason  = context.m_reason;
    answer.payload = context.m_payload;
    return answer;
}

addon::Usage_lifetime_summary lifetime_of(const Answer& answer, const addon::Account_context& account)
{
    check(!answer.threw && answer.status == addon::Result_status::SUCCESS,
        "the lifetime summary is read: " + answer.reason);
    addon::Contract_failure failure;
    auto summary = addon::decode_usage_lifetime_summary_result(answer.payload, &failure);
    check(summary.has_value(), "the lifetime summary satisfies its contract: " + failure.detail);
    check(addon::encode_account_context(summary->account_context) == addon::encode_account_context(account),
        "the lifetime summary echoes the exact account context");
    return std::move(*summary);
}

addon::Usage_saved_daily_tokens daily_of(const Answer& answer, const addon::Account_context& account)
{
    check(!answer.threw && answer.status == addon::Result_status::SUCCESS,
        "the saved daily tokens are read: " + answer.reason);
    addon::Contract_failure failure;
    auto summary = addon::decode_usage_saved_daily_tokens_result(answer.payload, &failure);
    check(summary.has_value(), "the saved daily tokens satisfy their contract: " + failure.detail);
    check(addon::encode_account_context(summary->account_context) == addon::encode_account_context(account),
        "the saved daily tokens echo the exact account context");
    return std::move(*summary);
}

std::string digest_of(std::string_view bytes)
{
    return addon::to_hex(addon::Sha256::digest(bytes));
}

// A stats cache in the shape Claude Code 2.1 writes, including members this
// package does not report. Models and days are out of order on purpose.
const std::string k_valid_cache = R"({
  "version": 5,
  "lastComputedDate": "2026-09-25",
  "dailyActivity": [{"date": "2026-09-24", "messageCount": 12, "sessionCount": 2, "toolCallCount": 7}],
  "dailyModelTokens": [
    {"date": "2026-09-24", "tokensByModel": {"claude-sonnet-4-5": 1200, "claude-opus-4-6": 3400}},
    {"date": "2026-09-22", "tokensByModel": {"claude-opus-4-6": 560}},
    {"date": "2026-09-23", "tokensByModel": {}}
  ],
  "dailyModelTokensVersion": 5,
  "modelUsage": {
    "claude-sonnet-4-5": {"inputTokens": 1500, "outputTokens": 700, "cacheReadInputTokens": 90000,
      "cacheCreationInputTokens": 4000, "webSearchRequests": 0, "costUSD": 0, "contextWindow": 0,
      "maxOutputTokens": 0},
    "claude-opus-4-6": {"inputTokens": 2500, "outputTokens": 1300, "cacheReadInputTokens": 250000,
      "cacheCreationInputTokens": 12000, "webSearchRequests": 3, "costUSD": 0, "contextWindow": 0,
      "maxOutputTokens": 0}
  },
  "totalSessions": 4,
  "totalMessages": 38,
  "hourCounts": {"9": 3, "14": 1}
})";

void valid_cache_contract(const fs::path& fixture)
{
    const auto profile = fixture / "valid" / ".claude";
    write(profile / "stats-cache.json", k_valid_cache);
    const auto account = account_for(profile);

    const auto lifetime = lifetime_of(read_saved("usage-lifetime-summary", account), account);
    check(lifetime.source_kind == "claude-stats-cache", "the lifetime source is Claude's stats cache");
    check(lifetime.source_content_digest == digest_of(k_valid_cache),
        "the lifetime digest hashes the exact cache bytes");
    check(lifetime.source_format_version == 5, "the lifetime summary reports the cache's own version");
    check(lifetime.cache_stated_date == std::optional<std::string>("2026-09-25"),
        "the lifetime summary reports the date the cache states");
    check(lifetime.historical_principal == "unknown", "the historical principal stays unknown");
    check(lifetime.models.size() == 2 &&
        lifetime.models[0].model == "claude-opus-4-6" && lifetime.models[1].model == "claude-sonnet-4-5",
        "every saved model is reported once, in byte order");
    const auto& opus = lifetime.models[0];
    check(opus.input_tokens == 2500 && opus.output_tokens == 1300 &&
        opus.cache_read_tokens == 250000 && opus.cache_creation_tokens == 12000,
        "each token class carries its own saved counter");

    const auto daily = daily_of(read_saved("usage-saved-daily-tokens", account), account);
    check(daily.source_content_digest == digest_of(k_valid_cache), "the daily digest hashes the exact cache bytes");
    check(daily.source_format_version == 5 && daily.daily_series_format_version == 5,
        "the daily tokens report both observed versions");
    check(daily.cache_stated_date == std::optional<std::string>("2026-09-25"),
        "the daily tokens report the date the cache states");
    check(daily.days.size() == 3 && daily.days[0].date == "2026-09-22" &&
        daily.days[1].date == "2026-09-23" && daily.days[2].date == "2026-09-24",
        "every saved day is reported once, in date order");
    check(daily.days[1].models.empty(), "a saved day without model rows stays a day without rows");
    const auto& last = daily.days[2].models;
    check(last.size() == 2 && last[0].model == "claude-opus-4-6" && last[0].tokens == 3400 &&
        last[1].model == "claude-sonnet-4-5" && last[1].tokens == 1200,
        "each day reports its per-model totals in byte order");
}

void missing_cache_contract(const fs::path& fixture)
{
    const auto profile = fixture / "missing" / ".claude";
    fs::create_directories(profile);
    const auto account = account_for(profile);
    for (const auto* capability : {"usage-lifetime-summary", "usage-saved-daily-tokens"}) {
        const auto answer = read_saved(capability, account);
        check(!answer.threw && answer.status == addon::Result_status::UNAVAILABLE &&
            answer.reason == "stats_cache_unavailable",
            std::string(capability) + " reports a profile without a stats cache as unavailable");
    }

    // A cache without a dated series still has lifetime totals.
    const auto undated = fixture / "undated" / ".claude";
    write(undated / "stats-cache.json",
        R"({"version":2,"modelUsage":{"claude-opus-4-6":{"inputTokens":1,"outputTokens":2,)"
        R"("cacheReadInputTokens":3,"cacheCreationInputTokens":4}}})");
    const auto undated_account = account_for(undated);
    const auto lifetime = lifetime_of(read_saved("usage-lifetime-summary", undated_account), undated_account);
    check(!lifetime.cache_stated_date && lifetime.models.size() == 1,
        "a cache without a stated date or dated series has lifetime totals");
    const auto daily = read_saved("usage-saved-daily-tokens", undated_account);
    check(!daily.threw && daily.status == addon::Result_status::UNAVAILABLE &&
        daily.reason == "stats_cache_daily_unavailable",
        "a cache without a dated series has no saved daily tokens");
}

// Each malformed cache fails the capabilities it concerns and leaves the
// other one readable.
void malformed_cache_contract(const fs::path& fixture)
{
    const std::string counters = R"("outputTokens":2,"cacheReadInputTokens":3,"cacheCreationInputTokens":4)";
    const std::string models = R"("modelUsage":{"claude-opus-4-6":{"inputTokens":1,)" + counters + "}}";
    const std::string days = R"("dailyModelTokens":[{"date":"2026-09-24","tokensByModel":{"claude-opus-4-6":10}}])";
    struct Malformed
    {
        const char* name;
        std::string bytes;
        bool        lifetime_fails;
        bool        daily_fails;
    };
    const std::vector<Malformed> cases = {
        {"truncated", "{" + models + "," + days, true, true},
        {"top level that is no object", R"([{"modelUsage":{}}])", true, true},
        {"stated date that is no date", "{\"lastComputedDate\":\"2026-02-30\"," + models + "," + days + "}", true, true},
        {"version that is no integer", "{\"version\":\"5\"," + models + "," + days + "}", true, true},
        {"no model usage", "{" + days + "}", true, false},
        {"negative counter", R"({"modelUsage":{"claude-opus-4-6":{"inputTokens":-1,)" + counters + "}}," + days + "}",
            true, false},
        {"fractional counter", R"({"modelUsage":{"claude-opus-4-6":{"inputTokens":1.5,)" + counters + "}}," + days + "}",
            true, false},
        {"missing counter", R"({"modelUsage":{"claude-opus-4-6":{"inputTokens":1}},)" + days + "}", true, false},
        {"empty model name", R"({"modelUsage":{"":{"inputTokens":1,)" + counters + "}}," + days + "}", true, false},
        {"counters past 2^53 - 1 together",
            R"({"modelUsage":{"claude-opus-4-6":{"inputTokens":9007199254740991,)" + counters + "}}," + days + "}",
            true, false},
        {"dated series that is no array", "{" + models + R"(,"dailyModelTokens":{}})", false, true},
        {"day without a date", "{" + models + R"(,"dailyModelTokens":[{"tokensByModel":{}}]})", false, true},
        {"day that is no date", "{" + models + R"(,"dailyModelTokens":[{"date":"2026-13-01","tokensByModel":{}}]})",
            false, true},
        {"day saved twice", "{" + models +
            R"(,"dailyModelTokens":[{"date":"2026-09-24","tokensByModel":{}},{"date":"2026-09-24","tokensByModel":{}}]})",
            false, true},
        {"daily total that is no integer", "{" + models +
            R"(,"dailyModelTokens":[{"date":"2026-09-24","tokensByModel":{"claude-opus-4-6":"10"}}]})", false, true},
        {"daily totals past 2^53 - 1 together", "{" + models +
            R"(,"dailyModelTokens":[{"date":"2026-09-24","tokensByModel":{"a":9007199254740991,"b":1}}]})",
            false, true},
    };
    std::size_t index = 0;
    for (const auto& malformed : cases) {
        const auto profile = fixture / "malformed" / std::to_string(index++) / ".claude";
        write(profile / "stats-cache.json", malformed.bytes);
        const auto account  = account_for(profile);
        const auto lifetime = read_saved("usage-lifetime-summary", account);
        const auto daily    = read_saved("usage-saved-daily-tokens", account);
        check(lifetime.threw == malformed.lifetime_fails,
            std::string("the lifetime summary ") + (malformed.lifetime_fails ? "fails" : "is read") +
                " for a cache with a " + malformed.name);
        check(daily.threw == malformed.daily_fails,
            std::string("the saved daily tokens ") + (malformed.daily_fails ? "fail" : "are read") +
                " for a cache with a " + malformed.name);
        if (!malformed.lifetime_fails) {
            (void)lifetime_of(lifetime, account);
        }
        if (!malformed.daily_fails) {
            (void)daily_of(daily, account);
        }
    }
}

// A read concerns only the profile the host selected: another profile's
// cache is never reported for it, and a context whose key belongs to another
// profile is refused.
void cross_profile_contract(const fs::path& fixture)
{
    const auto work     = fixture / "cross" / ".claude-work";
    const auto personal = fixture / "cross" / ".claude-personal";
    const auto fresh    = fixture / "cross" / ".claude";
    const std::string work_cache =
        R"({"modelUsage":{"claude-opus-4-6":{"inputTokens":10,"outputTokens":20,"cacheReadInputTokens":30,)"
        R"("cacheCreationInputTokens":40}},"dailyModelTokens":[{"date":"2026-09-20","tokensByModel":{"claude-opus-4-6":30}}]})";
    const std::string personal_cache =
        R"({"modelUsage":{"claude-sonnet-4-5":{"inputTokens":1,"outputTokens":2,"cacheReadInputTokens":3,)"
        R"("cacheCreationInputTokens":4}},"dailyModelTokens":[{"date":"2026-09-21","tokensByModel":{"claude-sonnet-4-5":3}}]})";
    write(work / "stats-cache.json", work_cache);
    write(personal / "stats-cache.json", personal_cache);
    fs::create_directories(fresh);

    const auto work_account     = account_for(work);
    const auto personal_account = account_for(personal);
    const auto work_lifetime     = lifetime_of(read_saved("usage-lifetime-summary", work_account), work_account);
    const auto personal_lifetime = lifetime_of(read_saved("usage-lifetime-summary", personal_account), personal_account);
    check(work_lifetime.source_content_digest == digest_of(work_cache) &&
        work_lifetime.models.size() == 1 && work_lifetime.models[0].model == "claude-opus-4-6",
        "the work profile reports only its own lifetime totals");
    check(personal_lifetime.source_content_digest == digest_of(personal_cache) &&
        personal_lifetime.models.size() == 1 && personal_lifetime.models[0].model == "claude-sonnet-4-5",
        "the personal profile reports only its own lifetime totals");
    const auto personal_daily = daily_of(read_saved("usage-saved-daily-tokens", personal_account), personal_account);
    check(personal_daily.days.size() == 1 && personal_daily.days[0].date == "2026-09-21",
        "the personal profile reports only its own saved days");

    const auto fresh_account = account_for(fresh);
    for (const auto* capability : {"usage-lifetime-summary", "usage-saved-daily-tokens"}) {
        const auto answer = read_saved(capability, fresh_account);
        check(!answer.threw && answer.status == addon::Result_status::UNAVAILABLE,
            std::string(capability) + " does not report a sibling profile's cache for a profile without one");
        auto borrowed = personal_account;
        borrowed.adapter_account_key = work_account.adapter_account_key;
        check(read_saved(capability, borrowed).threw,
            std::string(capability) + " refuses a context whose key belongs to another profile");
    }
}

// The packaged adapter as the host dispatches it: both capabilities are live,
// a read echoes the account context, and each outcome reaches the host with
// the status the in-process reads showed.
void packaged_dispatch_contract(const fs::path& package, const fs::path& fixture)
{
    const auto profile = fixture / "packaged" / ".claude";
    write(profile / "stats-cache.json", k_valid_cache);
    const auto missing = fixture / "packaged" / ".claude-missing";
    fs::create_directories(missing);
    const auto malformed = fixture / "packaged" / ".claude-malformed";
    write(malformed / "stats-cache.json", R"({"modelUsage":{"claude-opus-4-6":{"inputTokens":-1}}})");

    addon::Simulator_options options;
    options.package_directory    = package;
    options.profile_storage_root = fixture / "packaged" / "host-storage";
    addon::Host_simulator host(options, nullptr);
    addon::Contract_failure failure;
    check(host.start(&failure), "packaged adapter starts: " + failure.detail);
    const auto live = host.live_capabilities();
    for (const auto& pair : {addon::Capability_pair{"usage-lifetime-summary", 1},
                             addon::Capability_pair{"usage-saved-daily-tokens", 1}})
    {
        check(std::find(live.begin(), live.end(), pair) != live.end(),
            "the package negotiates " + pair.capability_id + "@1");
    }

    const auto dispatch = [&host](std::string_view capability, const addon::Account_context& account) {
        const bool lifetime = capability == "usage-lifetime-summary";
        const auto id = host.invoke({std::string(capability), 1}, "read", lifetime
            ? addon::encode_usage_lifetime_summary_request({account})
            : addon::encode_usage_saved_daily_tokens_request({account}), 10000);
        const auto outcome = host.wait_outcome(id, 11000);
        check(outcome.has_value(), "packaged " + std::string(capability) + " read finishes");
        return *outcome;
    };

    const auto account  = account_for(profile);
    const auto lifetime = dispatch("usage-lifetime-summary", account);
    check(lifetime.status == addon::Host_status::SUCCESS, "packaged lifetime read succeeds: " + lifetime.reason_code);
    check(!addon::account_context_acknowledgment_failure(account, lifetime.payload),
        "the packaged lifetime summary acknowledges the exact account context");
    const auto summary = addon::decode_usage_lifetime_summary_result(lifetime.payload, &failure);
    check(summary && summary->source_content_digest == digest_of(k_valid_cache) && summary->models.size() == 2,
        "the packaged lifetime summary carries the selected profile's totals");

    const auto daily = dispatch("usage-saved-daily-tokens", account);
    check(daily.status == addon::Host_status::SUCCESS, "packaged daily read succeeds: " + daily.reason_code);
    check(!addon::account_context_acknowledgment_failure(account, daily.payload),
        "the packaged saved daily tokens acknowledge the exact account context");
    const auto days = addon::decode_usage_saved_daily_tokens_result(daily.payload, &failure);
    check(days && days->days.size() == 3, "the packaged saved daily tokens carry the selected profile's days");

    const auto absent = dispatch("usage-lifetime-summary", account_for(missing));
    check(absent.status == addon::Host_status::UNAVAILABLE && absent.reason_code == "stats_cache_unavailable",
        "a packaged read of a profile without a stats cache is unavailable: " + absent.reason_code);
    const auto refused = dispatch("usage-lifetime-summary", account_for(malformed));
    check(refused.status == addon::Host_status::FAILED && refused.reason_code == "claude_operation_failed",
        "a packaged read of a malformed stats cache fails: " + refused.reason_code);

    check(host.shutdown(1000, nullptr), "packaged adapter exits by itself");
}

} // namespace

int main(int argc, char** argv)
{
    try {
        check(argc == 2, "usage summary tests require the built package directory");
        Temporary_directory fixture;
        valid_cache_contract(fixture.path());
        missing_cache_contract(fixture.path());
        malformed_cache_contract(fixture.path());
        cross_profile_contract(fixture.path());
        packaged_dispatch_contract(addon::path_from_utf8(argv[1]), fixture.path());
        std::cout << "Claude saved usage checks passed.\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
