#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/session_contracts.h>
#include <logonomic_cli_addon/sidecar.h>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <initializer_list>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

addon::Purposes_declaration read_declaration(const fs::path& package, const std::string& capability)
{
    const auto path = package / addon::sidecar_relative_path(capability, 3);
    std::ifstream input(path, std::ios::binary);
    require(input.is_open(), capability + ": cannot read packaged sidecar");
    const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    require(!input.bad(), capability + ": sidecar read failed");
    addon::Contract_failure failure;
    const auto sidecar = addon::parse_sidecar(bytes, &failure);
    require(sidecar.has_value(), capability + ": " + failure.reason_code + ": " + failure.detail);
    require(sidecar->capability_id == capability && sidecar->version == 3,
        capability + ": packaged sidecar identity mismatch");

    // The host decodes the whole declaration before admitting either purpose.
    // Package envelope validation alone does not establish this contract.
    const auto declaration = addon::decode_purposes_declaration(sidecar->payload, &failure);
    require(declaration.has_value(), capability + ": " + failure.reason_code + ": " + failure.detail);
    for (const auto purpose : {addon::Structured_purpose::READ_ONLY_REVIEW, addon::Structured_purpose::IMPLEMENTATION}) {
        require(std::find(declaration->purposes.begin(), declaration->purposes.end(), purpose) !=
            declaration->purposes.end(), capability + ": missing supported purpose " + addon::structured_purpose_name(purpose));
    }
    return *declaration;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        require(argc == 2, "Expected the actual Claude package directory");
        const auto package = addon::path_from_utf8(argv[1]);
        const auto structured = read_declaration(package, "structured-session");
        const auto delegated = read_declaration(package, "delegated-session");
        addon::Contract_failure failure;
        const bool dependency_valid = addon::validate_delegated_session_dependency(delegated, &structured, &failure);
        require(dependency_valid,
            "Packaged session dependency: " + failure.reason_code + ": " + failure.detail);
        std::cout << "PASS: packaged Claude session declarations satisfy SDK decoding and dependency admission\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
