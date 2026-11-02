cmake_minimum_required(VERSION 3.25)
cmake_policy(SET CMP0141 NEW)
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
# A build that adds this package as a subproject sets the normal variable
# LOGONOMIC_CLI_ADDON_EMBEDDED. That build owns the cache, try_compile()
# settings, the SDK and testing (the SDK's docs/package_contract.md,
# "Embedded build"), so only a standalone build sets them up here.
if(NOT LOGONOMIC_CLI_ADDON_EMBEDDED)
    set(CMAKE_FASTBUILD_CAPTURE_SYSTEM_ENV OFF CACHE BOOL "Do not capture the configure environment" FORCE)
    list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES CMAKE_FASTBUILD_CAPTURE_SYSTEM_ENV)
endif()
if(POLICY CMP0168)
    cmake_policy(SET CMP0168 NEW)
elseif(CMAKE_GENERATOR STREQUAL "FASTBuild")
    message(FATAL_ERROR "FASTBuild package builds require direct FetchContent population (CMP0168).")
endif()

project(logonomic_cli_claude VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(LOGONOMIC_CLI_ADDON_EMBEDDED)
    if(NOT TARGET logonomic_cli_addon_sdk::sdk)
        message(FATAL_ERROR
            "The embedding build must add the Logonomic CLI add-on SDK before this package.")
    endif()
else()
    set(LOGONOMIC_CLI_ADDON_SDK_SOURCE "" CACHE PATH "Local Logonomic CLI add-on SDK source override")
    set(LOGONOMIC_CLI_ADDON_SDK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    if(LOGONOMIC_CLI_ADDON_SDK_SOURCE)
        add_subdirectory("${LOGONOMIC_CLI_ADDON_SDK_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/sdk")
    else()
        include(FetchContent)
        FetchContent_Declare(logonomic_sdk_source
            GIT_REPOSITORY https://github.com/Varinomics/logonomic.git
            GIT_TAG master
            GIT_SHALLOW TRUE
            SOURCE_SUBDIR sdk/logonomic_cli_addon_sdk)
        FetchContent_MakeAvailable(logonomic_sdk_source)
    endif()
endif()

function(claude_target target)
    target_link_libraries(${target} PRIVATE logonomic_cli_addon_sdk::sdk)
    # The Windows API selection belongs to the platform, so a MinGW build gets
    # the same one as MSVC.
    if(WIN32)
        target_compile_definitions(${target} PRIVATE
            _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endfunction()

add_library(claude_native_context STATIC src/claude_native_context.cpp)
claude_target(claude_native_context)
if(WIN32)
    target_link_libraries(claude_native_context PRIVATE advapi32)
endif()

add_library(claude_quota_windows STATIC src/claude_quota_windows.cpp)
claude_target(claude_quota_windows)
target_include_directories(claude_quota_windows PUBLIC src)

add_library(claude_provider STATIC
    src/claude_conversation.cpp src/claude_cost.cpp
    src/claude_accounts.cpp src/claude_launcher.cpp src/claude_launcher_approval.cpp
    src/claude_lifecycle.cpp
    src/claude_preparation.cpp src/claude_probes.cpp src/claude_resume.cpp src/claude_sessions.cpp src/claude_usage.cpp
    src/claude_usage_summary.cpp src/claude_wake.cpp)
claude_target(claude_provider)
target_include_directories(claude_provider PUBLIC src)
target_link_libraries(claude_provider PUBLIC logonomic_cli_addon_sdk::sdk
    PRIVATE claude_native_context claude_quota_windows)
add_executable(claude_adapter src/adapter.cpp)
target_link_libraries(claude_adapter PRIVATE claude_provider)
claude_target(claude_adapter)
add_executable(claude_helper src/claude_helper.cpp)
target_link_libraries(claude_helper PRIVATE claude_provider)
claude_target(claude_helper)
add_executable(claude_launch_bridge src/claude_launch_bridge.cpp)
claude_target(claude_launch_bridge)
add_library(claude_quota_direct STATIC src/claude_quota_direct.cpp src/claude_quota_http.cpp)
claude_target(claude_quota_direct)
target_link_libraries(claude_quota_direct PRIVATE claude_native_context claude_quota_windows)
target_include_directories(claude_quota_direct PUBLIC src)
target_link_libraries(claude_launch_bridge PRIVATE claude_quota_direct)
if(WIN32)
    target_link_libraries(claude_quota_direct PRIVATE winhttp advapi32)
else()
    find_package(CURL REQUIRED)
    target_link_libraries(claude_quota_direct PRIVATE CURL::libcurl)
endif()
if(MINGW)
    # The bridge's Windows entry point is wmain, which MinGW links only with
    # -municode.
    target_link_options(claude_launch_bridge PRIVATE -municode)
endif()

logonomic_cli_addon_package_platform(CLAUDE_PLATFORM)
set(CLAUDE_EXECUTABLE_SUFFIX "${CMAKE_EXECUTABLE_SUFFIX}")
logonomic_cli_addon_package_directory(logonomic.cli.claude CLAUDE_PACKAGE_DIRECTORY)
configure_file(package/logonomic-cli-addon.json.in generated/logonomic-cli-addon.json @ONLY)
logonomic_cli_addon_package_indexer(claude_package_indexer claude_index_dependencies)
set(claude_index_command ${claude_package_indexer} "${CLAUDE_PACKAGE_DIRECTORY}"
    --third-party assets/claude.svg notices/lobe_icons_mit.txt provenance/lobe_icons.json)
set(claude_runtime_inputs "")
if(APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(CLAUDE_RUNTIME_NOTICES_DIRECTORY "" CACHE PATH
        "Copyright/license files for external runtimes, named <library filename>.txt")
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # The bridge also owns native fallback. Its HTTP runtime must travel with
    # the package, so absence of a system libcurl cannot break ordinary launch.
    foreach(target claude_adapter claude_helper claude_launch_bridge)
        set_target_properties(${target} PROPERTIES
            BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "$ORIGIN" INSTALL_RPATH_USE_LINK_PATH FALSE)
        target_link_options(${target} PRIVATE "LINKER:--disable-new-dtags")
    endforeach()
    configure_file(cmake/claude_package_runtime.cmake.in generated/package-runtime.cmake @ONLY)
    set(claude_index_command "${CMAKE_COMMAND}"
        "-DPACKAGE=${CLAUDE_PACKAGE_DIRECTORY}" "-DINDEX=$<TARGET_FILE:logonomic_cli_addon_index>"
        -P "${CMAKE_CURRENT_BINARY_DIR}/generated/package-runtime.cmake")
    list(APPEND claude_runtime_inputs "${CMAKE_CURRENT_BINARY_DIR}/generated/package-runtime.cmake"
        "$<TARGET_FILE:CURL::libcurl>")
elseif(APPLE)
    find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
    logonomic_cli_addon_macos_runtime_deployer(CLAUDE_MACOS_RUNTIME_DEPLOYER)
    foreach(target claude_adapter claude_helper claude_launch_bridge)
        target_link_options(${target} PRIVATE "LINKER:-headerpad_max_install_names")
    endforeach()
    configure_file(cmake/claude_package_macos_runtime.cmake.in generated/package-runtime.cmake.in @ONLY)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/package-runtime-$<CONFIG>.cmake"
        INPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/package-runtime.cmake.in")
    set(claude_index_command "${CMAKE_COMMAND}"
        "-DPACKAGE=${CLAUDE_PACKAGE_DIRECTORY}"
        -P "${CMAKE_CURRENT_BINARY_DIR}/generated/package-runtime-$<CONFIG>.cmake")
    list(APPEND claude_runtime_inputs "${CMAKE_CURRENT_BINARY_DIR}/generated/package-runtime-$<CONFIG>.cmake"
        "${CLAUDE_MACOS_RUNTIME_DEPLOYER}" "$<TARGET_FILE:CURL::libcurl>")
endif()
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
        "${CMAKE_CURRENT_BINARY_DIR}/generated/logonomic-cli-addon.json" "${CLAUDE_PACKAGE_DIRECTORY}/logonomic-cli-addon.json"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:claude_adapter>"
        "${CLAUDE_PACKAGE_DIRECTORY}/adapter/claude_adapter${CLAUDE_EXECUTABLE_SUFFIX}"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:claude_helper>"
        "${CLAUDE_PACKAGE_DIRECTORY}/helpers/claude_helper${CLAUDE_EXECUTABLE_SUFFIX}"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:claude_launch_bridge>"
        "${CLAUDE_PACKAGE_DIRECTORY}/helpers/claude_launch_bridge${CLAUDE_EXECUTABLE_SUFFIX}"
    COMMAND ${claude_index_command}
    DEPENDS claude_adapter claude_helper claude_launch_bridge logonomic_cli_addon_index
        "${CMAKE_CURRENT_BINARY_DIR}/generated/logonomic-cli-addon.json" "${CMAKE_CURRENT_BINARY_DIR}/package-inputs.txt"
        ${package_files} ${claude_runtime_inputs} ${claude_index_dependencies}
    VERBATIM)
add_custom_target(claude_package ALL DEPENDS "${CLAUDE_PACKAGE_DIRECTORY}/package.files.json")
logonomic_cli_addon_install_package("${CLAUDE_PACKAGE_DIRECTORY}" logonomic.cli.claude TARGET claude_package)

# Everything below is the package's own test suite, which only its standalone
# build configures; an embedding build stages the package and stops here.
if(LOGONOMIC_CLI_ADDON_EMBEDDED)
    return()
endif()

include(CTest)
option(LOGONOMIC_CLI_ADDON_LARGE_DATA_TESTS
    "Register full-size usage history and capacity regressions" OFF)
if(BUILD_TESTING)
    add_executable(claude_cost_tests tests/claude_cost_tests.cpp)
    claude_target(claude_cost_tests)
    target_link_libraries(claude_cost_tests PRIVATE claude_provider)
    add_dependencies(claude_cost_tests claude_package)
    add_test(NAME claude_cost_tests COMMAND claude_cost_tests "${CLAUDE_PACKAGE_DIRECTORY}")
    set_tests_properties(claude_cost_tests PROPERTIES TIMEOUT 60 LABELS "quota;usage;packaged-adapter")
    add_executable(claude_resume_tests tests/claude_resume_tests.cpp)
    claude_target(claude_resume_tests)
    target_link_libraries(claude_resume_tests PRIVATE claude_provider)
    add_dependencies(claude_resume_tests claude_package)
    add_test(NAME claude_resume_tests COMMAND claude_resume_tests "${CLAUDE_PACKAGE_DIRECTORY}")
    set_tests_properties(claude_resume_tests PROPERTIES TIMEOUT 60 LABELS "resume;packaged-adapter")
    add_test(NAME claude_resume_filesystem_tests
        COMMAND claude_resume_tests "${CLAUDE_PACKAGE_DIRECTORY}" --filesystem)
    set_tests_properties(claude_resume_filesystem_tests PROPERTIES
        TIMEOUT 60 SKIP_RETURN_CODE 77 LABELS "resume;filesystem-fixture")
    add_executable(claude_conversation_tests tests/claude_conversation_tests.cpp)
    target_link_libraries(claude_conversation_tests PRIVATE claude_provider)
    claude_target(claude_conversation_tests)
    add_test(NAME claude_conversation_tests COMMAND claude_conversation_tests)
    set_tests_properties(claude_conversation_tests PROPERTIES TIMEOUT 30)
    add_executable(claude_quota_direct_tests tests/claude_quota_direct_tests.cpp)
    claude_target(claude_quota_direct_tests)
    target_link_libraries(claude_quota_direct_tests PRIVATE claude_quota_direct claude_provider)
    add_test(NAME claude_quota_direct_tests COMMAND claude_quota_direct_tests)
    set_tests_properties(claude_quota_direct_tests PROPERTIES TIMEOUT 30 LABELS "quota")
    add_test(NAME claude_quota_direct_filesystem_tests COMMAND claude_quota_direct_tests --filesystem)
    set_tests_properties(claude_quota_direct_filesystem_tests PROPERTIES
        TIMEOUT 30 SKIP_RETURN_CODE 77 LABELS "quota;filesystem-fixture")
    add_executable(claude_session_declaration_tests tests/claude_session_declaration_tests.cpp)
    claude_target(claude_session_declaration_tests)
    add_dependencies(claude_session_declaration_tests claude_package)
    add_test(NAME claude_session_declaration_tests
        COMMAND claude_session_declaration_tests "${CLAUDE_PACKAGE_DIRECTORY}")
    set_tests_properties(claude_session_declaration_tests PROPERTIES
        TIMEOUT 30 LABELS "packaged-adapter;session-declarations")
    add_executable(claude_continuation_tests tests/claude_continuation_tests.cpp)
    target_link_libraries(claude_continuation_tests PRIVATE claude_provider)
    claude_target(claude_continuation_tests)
    add_test(NAME claude_continuation_tests COMMAND claude_continuation_tests)
    set_tests_properties(claude_continuation_tests PROPERTIES TIMEOUT 120 LABELS "protocol;continuation")
    add_executable(claude_contract_tests tests/claude_contract_tests.cpp)
    target_link_libraries(claude_contract_tests PRIVATE claude_provider)
    claude_target(claude_contract_tests)
    add_dependencies(claude_contract_tests claude_package)
    add_test(NAME claude_contract_tests COMMAND claude_contract_tests "${CLAUDE_PACKAGE_DIRECTORY}")
    add_test(NAME claude_reply_contract_tests
        COMMAND claude_contract_tests "${CLAUDE_PACKAGE_DIRECTORY}" --reply-evidence)
    add_test(NAME claude_launcher_approval_tests COMMAND claude_contract_tests --launcher-approval)
    add_test(NAME claude_context_state_tests COMMAND claude_contract_tests --context-state)
    add_test(NAME claude_quota_tests COMMAND claude_contract_tests --quota)
    set_tests_properties(claude_quota_tests PROPERTIES TIMEOUT 30 LABELS "quota")
    set_tests_properties(claude_launcher_approval_tests PROPERTIES TIMEOUT 30 LABELS "launcher-approval")
    add_executable(claude_fake_vnmctl tests/claude_fake_vnmctl.cpp)
    claude_target(claude_fake_vnmctl)
    add_executable(claude_fake_cli tests/claude_fake_cli.cpp)
    claude_target(claude_fake_cli)
    if(WIN32)
        target_link_libraries(claude_fake_cli PRIVATE shell32)
    endif()
    add_executable(claude_initial_prompt_delivery_tests tests/claude_initial_prompt_delivery_tests.cpp)
    target_link_libraries(claude_initial_prompt_delivery_tests PRIVATE claude_provider)
    claude_target(claude_initial_prompt_delivery_tests)
    add_test(NAME claude_initial_prompt_delivery_tests
        COMMAND claude_initial_prompt_delivery_tests "$<TARGET_FILE:claude_fake_cli>")
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME claude_helper_tests COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/claude_helper_tests.py"
        "$<TARGET_FILE:claude_helper>" "$<TARGET_FILE:claude_fake_vnmctl>"
        "${CLAUDE_PACKAGE_DIRECTORY}/adapter/claude_adapter${CLAUDE_EXECUTABLE_SUFFIX}"
        "${CLAUDE_PACKAGE_DIRECTORY}/assets/claude-hooks/scripts/logonomic-hook-relay.cjs")
    add_test(NAME claude_context_helper_tests COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/claude_helper_tests.py"
        "$<TARGET_FILE:claude_helper>" "$<TARGET_FILE:claude_fake_vnmctl>"
        "${CLAUDE_PACKAGE_DIRECTORY}/adapter/claude_adapter${CLAUDE_EXECUTABLE_SUFFIX}"
        "${CLAUDE_PACKAGE_DIRECTORY}/assets/claude-hooks/scripts/logonomic-hook-relay.cjs"
        --context-only)
    add_test(NAME claude_reply_helper_tests COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/claude_helper_tests.py"
        "$<TARGET_FILE:claude_helper>" "$<TARGET_FILE:claude_fake_vnmctl>"
        "${CLAUDE_PACKAGE_DIRECTORY}/adapter/claude_adapter${CLAUDE_EXECUTABLE_SUFFIX}"
        "${CLAUDE_PACKAGE_DIRECTORY}/assets/claude-hooks/scripts/logonomic-hook-relay.cjs"
        --reply-only)
    add_test(NAME claude_package_validation COMMAND logonomic_cli_addon_validate
        "${CLAUDE_PACKAGE_DIRECTORY}" --platform "${CLAUDE_PLATFORM}")
    add_test(NAME claude_package_conformance COMMAND logonomic_cli_addon_conformance
        "${CLAUDE_PACKAGE_DIRECTORY}")
    find_program(CLAUDE_NODE_EXECUTABLE node)
    if(CLAUDE_NODE_EXECUTABLE)
        add_test(NAME claude_wake_transport_tests COMMAND "${CLAUDE_NODE_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/claude_wake_transport_tests.cjs"
            "${CMAKE_CURRENT_SOURCE_DIR}/package/assets/claude-hooks/scripts/logonomic-wake.cjs")
        set_tests_properties(claude_wake_transport_tests PROPERTIES TIMEOUT 30)
        add_test(NAME claude_hook_contract_tests COMMAND "${CLAUDE_NODE_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/claude_hook_tests.mjs"
            "${CMAKE_CURRENT_SOURCE_DIR}/package/assets/claude-hooks/scripts/logonomic-hook-relay.cjs")
        add_test(NAME claude_context_hook_tests COMMAND "${CLAUDE_NODE_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/claude_context_hook_tests.mjs"
            "${CMAKE_CURRENT_SOURCE_DIR}/package/assets/claude-hooks/hooks/wake.js")
        set_tests_properties(claude_context_hook_tests PROPERTIES TIMEOUT 30)
    else()
        message(WARNING "Node is unavailable: claude_hook_contract_tests cannot qualify this package and will fail. Use BUILD_TESTING=OFF for a package-only build.")
        add_test(NAME claude_hook_contract_tests COMMAND "${CMAKE_COMMAND}" -E false)
        set_tests_properties(claude_hook_contract_tests PROPERTIES LABELS "missing-test-dependency")
    endif()
    set_tests_properties(claude_contract_tests claude_package_validation claude_package_conformance
        claude_hook_contract_tests claude_helper_tests claude_context_helper_tests claude_context_state_tests
        claude_reply_contract_tests claude_reply_helper_tests
        claude_initial_prompt_delivery_tests PROPERTIES TIMEOUT 120)
endif()
if(BUILD_TESTING)
    add_executable(claude_usage_cache_tests tests/claude_usage_cache_tests.cpp)
    target_link_libraries(claude_usage_cache_tests PRIVATE
        claude_provider logonomic_cli_addon_sdk::test_support)
    claude_target(claude_usage_cache_tests)
    add_dependencies(claude_usage_cache_tests claude_package)
    add_test(NAME claude_usage_cache_tests
        COMMAND claude_usage_cache_tests "${CLAUDE_PACKAGE_DIRECTORY}")
    set_tests_properties(claude_usage_cache_tests PROPERTIES TIMEOUT 180 LABELS "usage;cache;packaged-adapter")
    if(LOGONOMIC_CLI_ADDON_LARGE_DATA_TESTS)
        add_test(NAME claude_usage_large_data_tests
            COMMAND claude_usage_cache_tests "${CLAUDE_PACKAGE_DIRECTORY}" --large-data)
        set_tests_properties(claude_usage_large_data_tests PROPERTIES
            TIMEOUT 180 LABELS "usage;cache;large-data")
    endif()
    add_executable(claude_usage_host_tests tests/claude_usage_host_tests.cpp)
    target_link_libraries(claude_usage_host_tests PRIVATE claude_provider)
    claude_target(claude_usage_host_tests)
    add_dependencies(claude_usage_host_tests claude_package)
    add_test(NAME claude_usage_host_tests COMMAND claude_usage_host_tests "${CLAUDE_PACKAGE_DIRECTORY}")
    set_tests_properties(claude_usage_host_tests PROPERTIES TIMEOUT 180 LABELS "usage;cache;packaged-adapter")
    add_executable(claude_usage_summary_tests tests/claude_usage_summary_tests.cpp)
    target_link_libraries(claude_usage_summary_tests PRIVATE claude_provider)
    claude_target(claude_usage_summary_tests)
    add_dependencies(claude_usage_summary_tests claude_package)
    add_test(NAME claude_usage_summary_tests
        COMMAND claude_usage_summary_tests "${CLAUDE_PACKAGE_DIRECTORY}")
    set_tests_properties(claude_usage_summary_tests PROPERTIES TIMEOUT 120 LABELS "usage;packaged-adapter")
endif()

option(LOGONOMIC_CLI_ADDON_RECIPE_TESTS
    "Register isolated package rebuild regressions (requires Python 3)" OFF)
if(BUILD_TESTING AND LOGONOMIC_CLI_ADDON_RECIPE_TESTS)
    # Exercise this repository's actual producer, never a substitute packaging
    # implementation. The isolated nested configure uses this same SDK source;
    # it does not change the owned master dependency or edit an installed package.
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    get_target_property(_membership_sdk_source logonomic_cli_addon_sdk::sdk SOURCE_DIR)
    add_test(NAME claude_package_membership
        COMMAND "${Python3_EXECUTABLE}" "${_membership_sdk_source}/cmake/package_membership_tests.py"
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
