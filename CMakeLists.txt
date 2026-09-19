cmake_minimum_required(VERSION 3.25)
cmake_policy(SET CMP0141 NEW)
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT Embedded)
set(CMAKE_FASTBUILD_CAPTURE_SYSTEM_ENV OFF CACHE BOOL "Do not capture the configure environment" FORCE)
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES CMAKE_FASTBUILD_CAPTURE_SYSTEM_ENV)
if(POLICY CMP0168)
    cmake_policy(SET CMP0168 NEW)
elseif(CMAKE_GENERATOR STREQUAL "FASTBuild")
    message(FATAL_ERROR "FASTBuild package builds require direct FetchContent population (CMP0168).")
endif()

project(logonomic_cli_claude VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(LOGONOMIC_CLI_ADDON_SDK_SOURCE "" CACHE PATH "Local Logonomic CLI add-on SDK source override")
set(LOGONOMIC_CLI_ADDON_SDK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
if(LOGONOMIC_CLI_ADDON_SDK_SOURCE)
    add_subdirectory("${LOGONOMIC_CLI_ADDON_SDK_SOURCE}" "${CMAKE_BINARY_DIR}/sdk")
else()
    include(FetchContent)
    FetchContent_Declare(logonomic_sdk_source
        GIT_REPOSITORY https://github.com/Varinomics/logonomic.git
        GIT_TAG master
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR sdk/logonomic_cli_addon_sdk)
    FetchContent_MakeAvailable(logonomic_sdk_source)
endif()

function(claude_target target)
    target_link_libraries(${target} PRIVATE logonomic_cli_addon_sdk::sdk)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
        target_compile_definitions(${target} PRIVATE
            _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endfunction()

add_library(claude_provider STATIC
    src/claude_accounts.cpp src/claude_launcher.cpp src/claude_lifecycle.cpp
    src/claude_preparation.cpp src/claude_probes.cpp src/claude_sessions.cpp src/claude_usage.cpp)
claude_target(claude_provider)
target_include_directories(claude_provider PUBLIC src)
target_link_libraries(claude_provider PUBLIC logonomic_cli_addon_sdk::sdk)
add_executable(claude_adapter src/adapter.cpp)
target_link_libraries(claude_adapter PRIVATE claude_provider)
claude_target(claude_adapter)
add_executable(claude_helper src/claude_helper.cpp)
claude_target(claude_helper)
add_executable(claude_launch_bridge src/claude_launch_bridge.cpp)
claude_target(claude_launch_bridge)

logonomic_cli_addon_package_platform(CLAUDE_PLATFORM)
set(CLAUDE_EXECUTABLE_SUFFIX "${CMAKE_EXECUTABLE_SUFFIX}")
logonomic_cli_addon_package_directory(logonomic.cli.claude CLAUDE_PACKAGE_DIRECTORY)
configure_file(package/logonomic-cli-addon.json.in generated/logonomic-cli-addon.json @ONLY)
file(GLOB_RECURSE package_files CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/package/assets/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/package/capabilities/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/package/provenance/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/package/notices/*")
# Directory membership is an input too: removing a resource, or adding one
# with an older timestamp, must rebuild the indexed package. file(GENERATE)
# changes this file only when membership changes, preserving no-op builds.
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/package-inputs.txt"
    CONTENT "${package_files}\n")
add_custom_command(
    OUTPUT "${CLAUDE_PACKAGE_DIRECTORY}/package.files.json"
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${CLAUDE_PACKAGE_DIRECTORY}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${CLAUDE_PACKAGE_DIRECTORY}/adapter" "${CLAUDE_PACKAGE_DIRECTORY}/helpers"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/package/assets" "${CLAUDE_PACKAGE_DIRECTORY}/assets"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/package/capabilities" "${CLAUDE_PACKAGE_DIRECTORY}/capabilities"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/package/provenance" "${CLAUDE_PACKAGE_DIRECTORY}/provenance"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/package/notices" "${CLAUDE_PACKAGE_DIRECTORY}/notices"
    COMMAND "${CMAKE_COMMAND}" -E copy
        "${CMAKE_BINARY_DIR}/generated/logonomic-cli-addon.json" "${CLAUDE_PACKAGE_DIRECTORY}/logonomic-cli-addon.json"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:claude_adapter>"
        "${CLAUDE_PACKAGE_DIRECTORY}/adapter/claude_adapter${CLAUDE_EXECUTABLE_SUFFIX}"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:claude_helper>"
        "${CLAUDE_PACKAGE_DIRECTORY}/helpers/claude_helper${CLAUDE_EXECUTABLE_SUFFIX}"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:claude_launch_bridge>"
        "${CLAUDE_PACKAGE_DIRECTORY}/helpers/claude_launch_bridge${CLAUDE_EXECUTABLE_SUFFIX}"
    COMMAND "$<TARGET_FILE:logonomic_cli_addon_index>" "${CLAUDE_PACKAGE_DIRECTORY}"
        --third-party "assets/claude.svg" "notices/lobe_icons_mit.txt"
            "provenance/lobe_icons.json"
    DEPENDS claude_adapter claude_helper claude_launch_bridge logonomic_cli_addon_index
        "${CMAKE_BINARY_DIR}/generated/logonomic-cli-addon.json" "${CMAKE_CURRENT_BINARY_DIR}/package-inputs.txt" ${package_files}
    VERBATIM)
add_custom_target(claude_package ALL DEPENDS "${CLAUDE_PACKAGE_DIRECTORY}/package.files.json")
include(CTest)
if(BUILD_TESTING)
    add_executable(claude_contract_tests tests/claude_contract_tests.cpp)
    target_link_libraries(claude_contract_tests PRIVATE claude_provider)
    claude_target(claude_contract_tests)
    add_test(NAME claude_contract_tests COMMAND claude_contract_tests)
    add_executable(claude_fake_vnmctl tests/claude_fake_vnmctl.cpp)
    claude_target(claude_fake_vnmctl)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME claude_helper_tests COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/claude_helper_tests.py"
        "$<TARGET_FILE:claude_helper>" "$<TARGET_FILE:claude_fake_vnmctl>")
    add_test(NAME claude_package_validation COMMAND logonomic_cli_addon_validate
        "${CLAUDE_PACKAGE_DIRECTORY}" --platform "${CLAUDE_PLATFORM}")
    add_test(NAME claude_package_conformance COMMAND logonomic_cli_addon_conformance
        "${CLAUDE_PACKAGE_DIRECTORY}")
    find_program(CLAUDE_NODE_EXECUTABLE node)
    if(CLAUDE_NODE_EXECUTABLE)
        add_test(NAME claude_hook_contract_tests COMMAND "${CLAUDE_NODE_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/claude_hook_tests.mjs"
            "${CMAKE_CURRENT_SOURCE_DIR}/package/assets/claude-hooks/scripts/logonomic-hook-relay.cjs")
    else()
        message(WARNING "Node is unavailable: claude_hook_contract_tests cannot qualify this package and will fail. Use BUILD_TESTING=OFF for a package-only build.")
        add_test(NAME claude_hook_contract_tests COMMAND "${CMAKE_COMMAND}" -E false)
        set_tests_properties(claude_hook_contract_tests PROPERTIES LABELS "missing-test-dependency")
    endif()
    set_tests_properties(claude_contract_tests claude_package_validation claude_package_conformance
        claude_hook_contract_tests claude_helper_tests PROPERTIES TIMEOUT 120)
endif()
logonomic_cli_addon_install_package("${CLAUDE_PACKAGE_DIRECTORY}" logonomic.cli.claude)

option(LOGONOMIC_CLI_ADDON_RECIPE_TESTS
    "Register isolated package rebuild regressions (requires Python 3)" OFF)
if(BUILD_TESTING AND LOGONOMIC_CLI_ADDON_RECIPE_TESTS)
    # Exercise this repository's actual producer, never a substitute packaging
    # implementation. The isolated nested configure uses this same SDK source;
    # it does not change the owned master dependency or edit an installed package.
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    get_target_property(_membership_sdk_source logonomic_cli_addon_sdk::sdk SOURCE_DIR)
    add_test(NAME claude_package_membership
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/package_membership_tests.py"
            "--source=${CMAKE_CURRENT_SOURCE_DIR}" "--sdk=${_membership_sdk_source}"
            "--cmake=${CMAKE_COMMAND}" "--target=claude_package" "--package-id=logonomic.cli.claude"
            "--generator=${CMAKE_GENERATOR}" "--config=$<CONFIG>" "--build-type=${CMAKE_BUILD_TYPE}"
            "--generator-platform=${CMAKE_GENERATOR_PLATFORM}" "--toolset=${CMAKE_GENERATOR_TOOLSET}"
            "--toolchain=${CMAKE_TOOLCHAIN_FILE}" "--compiler=${CMAKE_CXX_COMPILER}"
            "--make-program=${CMAKE_MAKE_PROGRAM}" "--prefix-path=${CMAKE_PREFIX_PATH}"
            "--validator=$<TARGET_FILE:logonomic_cli_addon_validate>" "--platform=${CLAUDE_PLATFORM}")
    set_tests_properties(claude_package_membership PROPERTIES
        TIMEOUT 1800 RESOURCE_LOCK cli_addon_recipe_rebuild LABELS "cli-addon;delivery;recipe;owning-path")
endif()
# Contract and recipe tests must not be confused with real native qualification.
get_directory_property(_provider_tests TESTS)
if(_provider_tests)
    set_property(TEST ${_provider_tests} APPEND PROPERTY LABELS "cli-addon;provider-contract")
endif()
