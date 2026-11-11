#include "claude_accounts.h"
#include "claude_native_context_fixture.h"
#include "windows_filesystem_test_support.h"

#include <logonomic_cli_addon_test/usage_scan.h>

#include <logonomic_cli_addon/environment_bootstrap.h>
#include <logonomic_cli_addon/conversation_export.h>
#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/profile_storage.h>
#include <logonomic_cli_addon/usage_incremental.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace addon = logonomic::cli_addon;
namespace test = addon::test;
namespace fs = std::filesystem;

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

fs::path io_path(const fs::path& path)
{
    return addon::usage_cache_io_path(path.lexically_normal());
}

void write(const fs::path& path, const std::string& bytes)
{
    fs::create_directories(io_path(path.parent_path()));
    std::ofstream output(io_path(path), std::ios::binary | std::ios::trunc);
    output << bytes;
    output.close();
    require(output.good(), "native source fixture write failed");
}

std::string row(std::size_t index, unsigned tokens)
{
    const auto id = "session-" + std::to_string(index);
    return "{\"type\":\"assistant\",\"sessionId\":\"" + id +
        "\",\"timestamp\":\"2026-09-12T12:00:00Z\",\"message\":{\"id\":\"" + id +
        "\",\"stop_reason\":\"end_turn\",\"usage\":{\"input_tokens\":" + std::to_string(tokens) + "}}}\n";
}

void usage_contract(const fs::path& package, const fs::path& fixture, bool long_profile)
{
    auto profile = fixture / "profile";
    if (long_profile) {
        while (profile.native().size() < 280) {
            profile /= "native-profile-directory-component";
        }
    }
    const auto projects = profile / "projects";
    auto project = projects / "repository";
    std::vector<fs::path> sources{project /  "short.jsonl"};
    if (!long_profile) {
        project = projects / std::string(190 - projects.native().size(), 'p');
        const auto chats = project ;
        for (const auto length : {259, 260}) {
            const auto path = chats / (std::string(length - chats.native().size() - 7, 's') + ".jsonl");
            require(path.native().size() == (std::size_t)length, "exact native filename boundary");
            sources.push_back(path);
        }
        project = projects / std::string(210, 'd');
        sources.push_back(project /  "long-directory.jsonl");
        sources.push_back(project / "parent/subagents/nested.jsonl");
    }
    for (std::size_t index = 0; index < sources.size(); ++index) {
        write(sources[index], row(index, 11));
    }
    const auto profile_alias = profile / "unused" / "..";
    const auto native_profile = addon::path_to_utf8(profile_alias);
    const auto account = claude::account_key(profile_alias);

    addon::Simulator_options options;
    options.package_directory = package;
    options.profile_storage_root = fixture / "storage";
    addon::Host_simulator host(options, nullptr);
    addon::Contract_failure failure;
    require(host.start(&failure), failure.detail);
    std::string error;
    const auto allocated = addon::allocate_profile_storage_root(
        host.profile_storage_root(), "logonomic.cli.claude", &error, account);
    require(allocated.has_value(), error);
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
    const auto acknowledgment = host.invoke({"profile-storage", 1}, "acknowledge",
        addon::Json_value::object(), 5000, invocation);
    const auto acknowledged = host.wait_outcome(acknowledgment, 6000);
    require(acknowledged && acknowledged->status == addon::Host_status::SUCCESS,
        "adapter acknowledges native path fixture storage");

    const auto select = [&](const char* name, std::optional<addon::Usage_cache_reference> accepted) {
        const auto candidate = storage / name;
        fs::create_directories(io_path(candidate));
        request.cache_selection = addon::Usage_cache_selection{std::move(accepted), addon::path_to_utf8(candidate)};
    };
    const auto scan = [&](unsigned expected_tokens) {
        const auto page = test::usage_scan_pages(host, request, 3);
        require(page.records.size() == sources.size() &&
            test::usage_token_total(page) == sources.size() * expected_tokens && page.cache_candidate,
            "every short, long and nested transcript contributes its exact usage");
        for (const auto& record : page.records) {
            require(record.source_path && !record.source_path->starts_with("\\\\?\\"),
                "reported native source paths retain ordinary spelling");
        }
        return page;
    };

    request.mode = "repair";
    select("full", std::nullopt);
    const auto first = scan(11);
    request.mode = "incremental";
    select("unchanged", first.cache_candidate);
    const auto unchanged = scan(11);
    for (std::size_t index = 0; index < first.records.size(); ++index) {
        require(first.records[index].record_id == unchanged.records[index].record_id &&
            first.records[index].source_path == unchanged.records[index].source_path,
            "incremental replay preserves retained source and record identities");
    }
    const auto incremental = [&] {
        const auto id = host.invoke({"usage-incremental", 1}, "scan",
            addon::encode_usage_incremental_request(request), 5000);
        const auto outcome = host.wait_outcome(id, 6000);
        require(outcome && outcome->status == addon::Host_status::SUCCESS,
            "long source incremental update succeeds");
        const auto update = addon::decode_usage_incremental_result(outcome->payload, &failure);
        require(update.has_value(), failure.detail);
        return *update;
    };
    select("indexed", unchanged.cache_candidate);
    const auto indexed = incremental();
    require(indexed.changes && indexed.changes->cache_candidate, "native sources acquire an incremental index");
    select("index-unchanged", indexed.changes->cache_candidate);
    require(!incremental().changes, "unchanged long native sources reuse their stable index identities");
    for (std::size_t index = 0; index < sources.size(); ++index) {
        write(sources[index], row(index, 123));
    }
    select("index-changed", indexed.changes->cache_candidate);
    const auto changed = incremental();
    require(changed.changes && changed.changes->cache_candidate &&
        changed.changes->records.size() == sources.size(), "changed long native sources update every session");
    select("changed", changed.changes->cache_candidate);
    scan(123);
    require(host.shutdown(1000, nullptr), "native path fixture adapter exits");
    std::cout << "claude native usage: " << (long_profile ? "long profile" : "259/260 and long directories")
              << " passed\n";
}

void conversation_contract(
    const fs::path& package, const fs::path& fixture, bool long_profile, bool resume, bool unc_profile)
{
    using Json = addon::Json_value;
    auto profile = fixture / "profile";
    if (long_profile) {
        while (profile.native().size() < 280) {
            profile /= "native-profile-directory-component";
        }
    }
    if (unc_profile) {
        const auto drive = profile.root_name().string();
        require(drive.size() == 2 && drive[1] == ':', "UNC fixture requires a local drive");
        profile = fs::path("\\\\localhost\\" + drive.substr(0, 1) + "$") / profile.relative_path();
    }
    const auto working_directory = fixture / "work";
    fs::create_directories(working_directory);
    const auto* unavailable = claude::test::native_context_unavailable(working_directory);
    require(!unavailable, unavailable ? unavailable : "");
    const auto program = fixture / "npm/bin/claude.exe";
    write(fixture / "npm/package.json",
        R"({"name":"@anthropic-ai/claude-code","bin":{"claude":"bin/claude.exe"}})");
    write(program, "MZfixture");
    write(fixture / "npm/bin/node.exe", "");
    write(profile / "settings.json", "{}");
    const auto profile_alias = profile / "unused" / "..";
    const auto native_profile = addon::path_to_utf8(profile_alias);
    std::string project = addon::path_to_utf8(working_directory);
    for (auto& byte : project) {
        if (!((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9')))
        {
            byte = '-';
        }
    }
    if (!long_profile) {
        project = std::string(210, 'p');
    }
    const std::string session = "d94ace27-2a10-48ec-9847-33ef223207f9";
    const auto source = profile / "projects" / project / (session + ".jsonl");
    require(source.native().size() > 260, "resume fixture exceeds the ordinary Windows path limit");
    write(source,
        "{\"type\":\"user\",\"uuid\":\"c8902599-6448-48be-9ca4-d67456950fae\","
        "\"parentUuid\":null,\"sessionId\":\"" + session +
        "\",\"message\":{\"role\":\"user\",\"content\":\"Retained conversation\"}}\n");

    addon::Simulator_options options;
    options.package_directory = package;
    addon::Host_simulator host(options, nullptr);
    addon::Contract_failure failure;
    require(host.start(&failure), failure.detail);
    addon::Resume_inspect_request inspection;
    inspection.context.launcher = {"claude", addon::k_runner_direct, {}};
    inspection.context.launcher_argv = {addon::path_to_utf8(program)};
    inspection.context.working_directory = working_directory.generic_string();
    addon::Launch_environment environment;
    environment.platform_id = addon::host_platform_id();
    environment.effective_environment = Json::object();
    environment.effective_environment.set("PATH", Json(addon::path_to_utf8(program.parent_path())));
    environment.effective_environment.set("CLAUDE_CONFIG_DIR", Json(native_profile));
    environment.effective_environment.set("USERPROFILE", Json(addon::path_to_utf8(fixture / "home")));
    inspection.context.launch_environment = environment;
    inspection.resume_record = Json::object();
    inspection.resume_record.set("provider_session_id", Json(session));
    const auto inspect = [&](addon::Resume_availability availability, addon::Resume_content content) {
        const auto id = host.invoke({"interactive-resume", 4}, "inspect_resume",
            addon::encode_resume_inspect_request(inspection), 5000);
        const auto outcome = host.wait_outcome(id, 6000);
        require(outcome && outcome->status == addon::Host_status::SUCCESS, "native resume inspection succeeds");
        const auto result = addon::decode_resume_inspect_result(outcome->payload, &failure, 4);
        require(result && result->availability == availability && result->content == content,
            "long native transcript has the expected availability and content: " +
                addon::serialize_json(outcome->payload));
    };
    if (resume) {
        inspect(addon::Resume_availability::AVAILABLE, addon::Resume_content::NONEMPTY);
        addon::Resume_prepare_request preparation;
        preparation.context = inspection.context;
        preparation.resume_record = inspection.resume_record;
        preparation.title_instruction = "Set the session title.";
        const auto prepared_id = host.invoke({"interactive-resume", 2}, "prepare_resume",
            addon::encode_resume_prepare_request(preparation, 2), 5000);
        const auto prepared = host.wait_outcome(prepared_id, 6000);
        require(prepared && prepared->status == addon::Host_status::SUCCESS,
            "long native transcript permits exact-session resume preparation");
        {
            filesystem_test::Access_denial denial(io_path(source), FILE_READ_DATA);
            inspect(addon::Resume_availability::UNKNOWN, addon::Resume_content::UNKNOWN);
        }
        require(!filesystem_test::restoration_failed, "fixture permissions restored");
        fs::remove(io_path(source));
        inspect(addon::Resume_availability::ABSENT, addon::Resume_content::EMPTY);
    }
    else {
        addon::Conversation_export_request export_request;
        export_request.provider_session_id = session;
        export_request.launch_environment = environment;
        const auto output = fixture / "output";
        fs::create_directories(output);
        export_request.output_directory = addon::path_to_utf8(fs::canonical(output));
        const auto export_id = host.invoke({"conversation-export", 1}, "export",
            addon::encode_conversation_export_request(export_request), 5000);
        const auto exported = host.wait_outcome(export_id, 6000);
        require(exported && exported->status == addon::Host_status::SUCCESS,
            "long native transcript export failed: " + (exported ? exported->reason_code : "timeout"));
        const auto result = addon::decode_conversation_export_result(exported->payload, &failure);
        require(result.has_value(), failure.detail);
        std::ifstream input(output / result->file_name, std::ios::binary);
        const std::string bytes(std::istreambuf_iterator<char>(input), {});
        require(bytes.find("Retained conversation") != std::string::npos, "export preserves native conversation text");
    }
    require(host.shutdown(1000, nullptr), "conversation fixture adapter exits");
    std::cout << "Claude native " << (resume ? "resume: " : "export: ")
              << (long_profile ? "long profile" : "long directory")
              << " passed\n";
}

} // namespace

int main(int argc, char** argv)
{
    const auto root = fs::temp_directory_path() / ("claude-native-paths-" + addon::random_nonce_hex());
    int result = 0;
    try {
        require(argc == 2 || argc == 3, "expected built package directory [--resume|--export|-unc variant]");
        fs::create_directories(root);
        const auto fixture = fs::canonical(root);
        const auto package = addon::path_from_utf8(argv[1]);
        if (argc == 3) {
            const std::string mode = argv[2];
            require(mode == "--resume" || mode == "--export" || mode == "--resume-unc" || mode == "--export-unc",
                "unknown native path fixture mode");
            const bool resume = mode.starts_with("--resume");
            const bool unc_profile = mode.ends_with("-unc");
            conversation_contract(package, fixture / "conversation-files", false, resume, unc_profile);
            conversation_contract(package, fixture / "conversation-profile", true, resume, unc_profile);
        }
        else {
            usage_contract(package, fixture / "files", false);
            usage_contract(package, fixture / "profile", true);
        }
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    std::error_code error;
    fs::remove_all(io_path(root), error);
    if (error) {
        std::cerr << "native path fixture cleanup: " << error.message() << '\n';
        result = 1;
    }
    return result;
}
