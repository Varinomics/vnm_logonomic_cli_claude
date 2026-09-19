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
function fake_spawn(response, exit_code = 0)
{
    return (program, args, options) => {
        assert.equal(path.basename(program), process.platform === "win32" ? "claude_helper.exe" : "claude_helper");
        assert.deepEqual(args, ["event", "--watch-parent"]);
        assert.equal(options.windowsHide, true);
        const child = new EventEmitter();
        child.stdin = new PassThrough();
        child.stdout = new PassThrough();
        child.stdin.once("data", (bytes) => {
            assert.equal(JSON.parse(bytes).hook_event_name, "UserPromptSubmit");
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
console.log("Claude hook response contract checks passed.");
