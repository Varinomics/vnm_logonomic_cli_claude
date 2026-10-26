#include "claude_launcher_approval.h"

namespace claude {

const logonomic::cli_addon::Launcher_approval_rules& launcher_approval_rules()
{
    static const logonomic::cli_addon::Launcher_approval_rules rules{
        .program_names = {"claude", "claude.exe", "claude.cmd", "claude.bat", "claude.ps1"},
        .canonical_argument = "--dangerously-skip-permissions",
        .bypass_arguments = {"--dangerously-skip-permissions"},
        .approval_modes = {
            {"--permission-mode", {"bypassPermissions"},
                {"default", "manual", "acceptEdits", "plan", "auto", "dontAsk"}},
        },
        .conflicting_arguments = {},
        // Variadic options are omitted: removing an approval flag can make
        // their values absorb a following prompt.
        .value_arguments = {
            "--model", "--fallback-model", "--agent", "--agents", "--effort",
            "--settings", "--setting-sources", "--system-prompt", "--system-prompt-file",
            "--append-system-prompt", "--append-system-prompt-file",
            "--plugin-dir", "--plugin-url", "--session-id", "--name", "-n",
            "--input-format", "--output-format", "--json-schema", "--max-turns",
            "--max-budget-usd", "--permission-prompt-tool", "--permission-prompts",
        },
        .optional_value_arguments = {"-r", "--resume", "-w", "--worktree"},
        .flag_arguments = {
            "-p", "--print", "-c", "--continue", "--allow-dangerously-skip-permissions",
            "--verbose", "--disable-slash-commands", "--strict-mcp-config", "--fork-session",
            "--no-session-persistence", "--chrome", "--no-chrome", "--ide",
            "--include-partial-messages", "--replay-user-messages",
        },
        .session_commands = {},
        .unsupported_commands = {
            "update", "install", "auth", "agents", "attach", "auto-mode", "daemon", "doctor",
            "gateway", "import", "logs", "mcp", "plugin", "plugins", "purge", "remote-control",
            "rm", "self-hosted-runner", "setup-token", "stop", "kill", "ultrareview", "help",
        },
    };
    return rules;
}

} // namespace claude
