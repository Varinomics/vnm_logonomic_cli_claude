"use strict";

const child_process = require("node:child_process");
const path = require("node:path");

const max_input_bytes = 4 * 1024 * 1024;
const max_output_bytes = 64 * 1024;
const event_budgets = Object.freeze({
    SessionStart: 17000,
    UserPromptSubmit: 17000,
    SubagentStart: 12000,
    SubagentStop: 12000,
    Stop: 12000,
    ModContext: 12000,
    ModTurnComplete: 12000,
    StopFailure: 12000,
    PermissionRequest: 12000,
    PreToolUse: 12000,
    PostToolUse: 12000,
    PostToolUseFailure: 12000,
    SessionEnd: 4000,
});

function relay_hook(payload, options = {})
{
    const environment = options.environment || process.env;
    const spawn = options.spawn || child_process.spawn;
    const event = payload && typeof payload === "object" ? payload.hook_event_name : "";
    if (!Object.hasOwn(event_budgets, event)) return Promise.resolve("");
    const directory = environment.LOGONOMIC_PROVIDER_HELPERS_DIR || "";
    if (!path.isAbsolute(directory) || !environment.LOGONOMIC_SESSION_CAPABILITY) return Promise.resolve("");
    // Ordinary child tools produce neither model context nor a lifecycle event.
    if (event === "PreToolUse" && Object.hasOwn(payload, "agent_id") &&
        payload.tool_name !== "AskUserQuestion" && payload.tool_name !== "ExitPlanMode") {
        return Promise.resolve("");
    }
    const executable = path.join(directory, process.platform === "win32" ? "claude_helper.exe" : "claude_helper");
    // Native ownership comes from the Mod's direct helper invocation. A Node
    // launcher may interpose here, so neither its PID nor payload data owns it.
    const owned_payload = { ...payload };
    delete owned_payload.native_owner_pid;
    const input = JSON.stringify(owned_payload);
    if (Buffer.byteLength(input) > max_input_bytes) return Promise.resolve("");
    return new Promise((resolve) => {
        let child;
        try {
            child = spawn(executable, ["event", "--watch-parent"], {
                env: environment,
                stdio: ["pipe", "pipe", "ignore"],
                windowsHide: true,
            });
        }
        catch (_) {
            resolve("");
            return;
        }
        const output = [];
        let output_bytes = 0;
        let cancelled = false;
        let failed = false;
        const cancel = () => {
            cancelled = true;
            // EOF asks the helper to cancel and reap its own vnmctl wrapper.
            // Completion is the child's close event, never the timer itself.
            if (!child.stdin.destroyed) child.stdin.end();
        };
        const timer = setTimeout(cancel, event_budgets[event]);
        child.stdout.on("data", (chunk) => {
            output_bytes += chunk.length;
            if (output_bytes > max_output_bytes) cancel();
            else output.push(chunk);
        });
        child.stdin.on("error", () => { failed = true; });
        child.on("error", () => { failed = true; });
        child.on("close", (code) => {
            clearTimeout(timer);
            if (cancelled || failed || code !== 0 || output_bytes > max_output_bytes) {
                resolve("");
                return;
            }
            const bytes = Buffer.concat(output).toString("utf8");
            if (!bytes.trim()) {
                resolve("");
                return;
            }
            try {
                const response = JSON.parse(bytes);
                const hook = response.hookSpecificOutput;
                if (!hook || hook.hookEventName !== event || typeof hook.additionalContext !== "string") {
                    resolve("");
                    return;
                }
                resolve(JSON.stringify(response) + "\n");
            }
            catch (_) {
                resolve("");
            }
        });
        child.stdin.write(input + "\n");
    });
}

async function main()
{
    const chunks = [];
    let size = 0;
    for await (const chunk of process.stdin) {
        size += chunk.length;
        if (size > max_input_bytes) return;
        chunks.push(chunk);
    }
    try {
        const payload = JSON.parse(Buffer.concat(chunks).toString("utf8"));
        process.stdout.write(await relay_hook(payload));
    }
    catch (_) {
        // Claude hooks are best effort; never expose a private hook payload.
    }
}

module.exports = { relay_hook, event_budgets };
if (require.main === module) main();
