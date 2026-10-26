import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';

const source = await readFile(process.argv[2], 'utf8');
let instance = 0;

async function fixture({ usage = { context: { tokens: 64000, window: 200000 } }, wake = false } = {})
{
    const handlers = new Map();
    const events = [];
    let session_id = 'native-session';
    const $ = {
        session: {
            id: async () => session_id,
            usage: async () => {
                if (usage instanceof Error) throw usage;
                return usage;
            },
        },
        env: { get: async key => wake && key === 'VNM_CLAUDE_WAKE_DIRECTORY' ? '/wake' : undefined },
        plugin: { root: '/plugin' },
        clock: { after() {} },
        process: { run: async (argv, options) => {
            assert.equal(argv[1], '/plugin/scripts/logonomic-hook-relay.cjs');
            events.push(JSON.parse(options.stdin));
            return { exitCode: 0, stdout: '', stderr: '' };
        } },
    };
    const module = await import('data:text/javascript;base64,' + Buffer.from(source).toString('base64') + '#' + instance++);
    module.register((name, handler) => {
        handlers.set(name, handler);
        return { catch() {} };
    });
    return {
        events,
        set_session: value => { session_id = value; },
        emit: async (name, event, next = async () => ({ unchanged: true })) => handlers.get(name)($, event, next),
    };
}

// Oracle: Claude's public session.usage projection supplies tokens/window only
// with native input usage; a fresh session supplies window alone. The plugin
// must preserve the native event result and omit unknown measurements/counts.
for (const [usage, expected] of [
    [{ context: { tokens: 64000, window: 200000 }, cost: { usd: 37 } }, { used_tokens: 64000, context_window: 200000 }],
    [{ context: { window: 200000 } }, { usage_unavailable: true }],
    [{ context: { tokens: null, window: 200000 } }, { usage_unavailable: true }],
    [{ context: { tokens: 0, window: 200000 } }, { used_tokens: 0, context_window: 200000 }],
    [{ context: { tokens: -1, window: 200000 } }, { usage_unavailable: true }],
    [{ context: { tokens: 64000.5, window: 200000 } }, { usage_unavailable: true }],
    [{ context: { tokens: 64000, window: 0 } }, { usage_unavailable: true }],
    [new Error('native usage unavailable'), { usage_unavailable: true }],
]) {
    const test = await fixture({ usage });
    const result = await test.emit('classic.SessionStart', { source: 'startup', session_id: 'native-session' });
    assert.deepEqual(result, { unchanged: true });
    assert.deepEqual(test.events, [{ hook_event_name: 'ModContext', session_id: 'native-session', context_state: expected }]);
}

for (const event of [
    { source: 'compact', session_id: 'native-session' },
    { source: 'startup', session_id: 'native-session', agent_id: 'child' },
]) {
    const test = await fixture();
    await test.emit('classic.SessionStart', event);
    assert.deepEqual(test.events, []);
}

for (const reason of ['answer', 'aborted', 'error']) {
    const test = await fixture();
    await test.emit('turn.complete', { reason });
    assert.equal(test.events.length, 1);
    assert.equal(test.events[0].hook_event_name, 'ModContext');
    assert.deepEqual(test.events[0].context_state, { used_tokens: 64000, context_window: 200000 });
}

{
    const test = await fixture();
    await test.emit('turn.complete', { reason: 'answer', agentId: 'child' });
    assert.deepEqual(test.events, []);
    await test.emit('turn.complete', { reason: 'answer' }, async () => {
        test.set_session('replacement-session');
        return { unchanged: true };
    });
    assert.deepEqual(test.events, []);
}

for (const changed_before of [false, true]) {
    const test = await fixture({ wake: true });
    await test.emit('session.start', { isInteractive: true });
    if (changed_before) test.set_session('replacement-session');
    await test.emit('turn.complete', { reason: 'answer' });
    assert.equal(test.events.length, changed_before ? 0 : 1);
    if (!changed_before) assert.equal(test.events[0].hook_event_name, 'ModTurnComplete');
}

{
    const test = await fixture();
    await assert.rejects(test.emit('turn.complete', { reason: 'answer' }, async () => {
        throw new Error('native completion failure');
    }), /native completion failure/);
    assert.deepEqual(test.events, []);
}

console.log('Claude context hook checks passed.');
