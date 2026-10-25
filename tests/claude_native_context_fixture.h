#pragma once

#include "claude_native_context.h"

#ifndef _WIN32
#include <sys/utsname.h>
#endif

#include <filesystem>
#include <string>

namespace claude::test {

inline const char* native_context_unavailable(const std::filesystem::path& working_directory)
{
#ifndef _WIN32
    utsname kernel{};
    if (uname(&kernel) != 0) {
        return "kernel policy context is unavailable";
    }
    std::string release = kernel.release;
    for (auto& byte : release) {
        if (byte >= 'A' && byte <= 'Z') {
            byte += 'a' - 'A';
        }
    }
    if (release.find("microsoft") != std::string::npos) {
        return "WSL retains native Windows-policy inheritance authority";
    }
#endif
    // A temporary profile cannot isolate machine-wide policy or CCR handoff
    // files. Only the filesystem-specific tests depend on their absence.
    if (!native_context::policy_absent()) {
        return "native managed policy context is unresolved";
    }
    if (!native_context::absent(working_directory.root_path() / "home/claude/.claude/remote")) {
        return "native remote handoff context is unresolved";
    }
    return nullptr;
}

} // namespace claude::test
