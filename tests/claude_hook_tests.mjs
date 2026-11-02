import assert from "node:assert/strict";
import { createRequire } from "node:module";
import { EventEmitter } from "node:events";
import { PassThrough } from "node:stream";
import path from "node:path";

const require = createRequire(import.meta.url);
const { relay_hook } = require(path.resolve(process.argv[2]));
const environment = {
    LOGONOMIC_PROVIDER_HELPERS_DIR: path.resolve("helpers"),
    LOGONOMIC_SESSION_CAPABILITY: "bound-session",
};
function fake_spawn(response, exit_code = 0, event_name = "UserPromptSubmit", observe = () => {})
{
    return (program, args, options) => {
        assert.equal(path.basename(program), process.platform === "win32" ? "claude_helper.exe" : "claude_helper");
        assert.deepEqual(args, ["event", "--watch-parent"]);
        assert.equal(options.windowsHide, true);
        const child = new EventEmitter();
        child.stdin = new PassThrough();
        child.stdout = new PassThrough();
        child.stdin.once("data", (bytes) => {
            const event = JSON.parse(bytes);
            assert.equal(event.hook_event_name, event_name);
            observe(event);
            queueMicrotask(() => {
                child.stdout.emit("data", Buffer.from(response));
                child.emit("close", exit_code);
            });
        });
        return child;
    };
}
const payload = { hook_event_name: "UserPromptSubmit", session_id: "SessionCase", prompt: "Show my changes" };
const accepted = JSON.stringify({ hookSpecificOutput: { hookEventName: "UserPromptSubmit", additionalContext: "Host-authorized instruction" } });
assert.equal(await relay_hook(payload, { environment, spawn: fake_spawn(accepted) }), accepted + "\n");
assert.equal(await relay_hook(payload, { environment, spawn: fake_spawn(accepted, 1) }), "");
assert.equal(await relay_hook(payload, { environment, spawn: fake_spawn('{"accepted":true}') }), "");
assert.equal(await relay_hook(payload, { environment, spawn: fake_spawn("x".repeat(65537)) }), "");
assert.equal(await relay_hook(payload, { environment: {}, spawn() { throw new Error("must not spawn"); } }), "");
const checkpoint = { hook_event_name: "PreToolUse", session_id: "SessionCase", tool_name: "Read" };
let root_helper_calls = 0;
for (const instruction of ["Fresh context 1", "Fresh context 2"]) {
    const response = JSON.stringify({ hookSpecificOutput: { hookEventName: "PreToolUse", additionalContext: instruction } });
    assert.equal(await relay_hook(checkpoint, {
        environment, spawn: fake_spawn(response, 0, "PreToolUse", event => {
            ++root_helper_calls;
            assert.deepEqual(event, checkpoint);
        }),
    }), response + "\n");
}
assert.equal(root_helper_calls, 2);
for (const agent_id of ["child", "", null, false, 0, [], {}]) {
    let helper_spawns = 0;
    assert.equal(await relay_hook({ ...checkpoint, agent_id }, {
        environment, spawn() { ++helper_spawns; throw new Error("unexpected helper spawn"); },
    }), "");
    assert.equal(helper_spawns, 0, `ordinary PreToolUse with agent_id=${JSON.stringify(agent_id)}`);
}
for (const event of [
    { ...checkpoint, agent_id: "child", tool_name: "AskUserQuestion" },
    { ...checkpoint, agent_id: "child", tool_name: "ExitPlanMode" },
    { ...checkpoint, agent_id: "child", hook_event_name: "PostToolUse" },
    { ...payload, agent_id: "child" },
]) {
    let helper_calls = 0;
    assert.equal(await relay_hook(event, {
        environment, spawn: fake_spawn("", 0, event.hook_event_name, forwarded => {
            ++helper_calls;
            assert.deepEqual(forwarded, event);
        }),
    }), "");
    assert.equal(helper_calls, 1, `${event.hook_event_name} ${event.tool_name || ""} must reach the helper`);
}
for (const event_name of ["SessionStart", "SessionEnd"]) {
    assert.equal(await relay_hook({ hook_event_name: event_name, native_owner_pid: -1 }, {
        environment, spawn: fake_spawn("", 0, event_name, event => assert.equal(event.native_owner_pid, undefined)),
    }), "");
}
assert.equal(await relay_hook({ hook_event_name: "ModTurnComplete", session_id: "SessionCase", reason: "answer", native_owner_pid: -1 }, {
    environment, spawn: fake_spawn("", 0, "ModTurnComplete", event => {
        assert.equal(event.reason, "answer");
        assert.equal(event.native_owner_pid, undefined);
    }),
}), "");
console.log("Claude hook response contract checks passed.");
