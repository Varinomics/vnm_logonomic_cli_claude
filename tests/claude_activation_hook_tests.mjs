import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';

const sources = await Promise.all(process.argv.slice(2).map(path => readFile(path, 'utf8')));
assert.equal(sources.length, 2, 'admission and dispatcher modules are required');
let instance = 0;

function deferred() {
    let resolve;
    let reject;
    const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
    return { promise, resolve, reject };
}

async function fixture(source) {
    const handlers = new Map();
    const scheduled = [];
    const events = [];
    const operations = [];
    let session_id = 'A';
    let usage = async () => ({ context: { tokens: 12, window: 100 } });
    let environment = async key => key === 'VNM_CLAUDE_WAKE_DIRECTORY' ? '/private/wake' : undefined;
    let transport = async operation => {
        if (operation === 'binding') return { provider_session_id: session_id, transition_nonce: 'b'.repeat(32) };
        if (operation === 'ready') return { completed: true };
        throw new Error('fixture listener stopped');
    };
    const $ = {
        session: { id: async () => session_id, usage: () => usage() },
        env: { get: key => environment(key) },
        plugin: { root: '/plugin' },
        clock: { after: (delay, callback) => scheduled.push(callback) },
        process: { run: async (argv, options) => {
            if (argv[1].endsWith('/logonomic-hook-relay.cjs')) {
                events.push(JSON.parse(options.stdin));
                return { exitCode: 0, stdout: '', stderr: '' };
            }
            const operation = argv[2];
            operations.push({ operation, value: options.stdin ? JSON.parse(options.stdin) : undefined });
            const result = await transport(operation);
            return { exitCode: 0, stdout: JSON.stringify(result), stderr: '' };
        } },
    };
    const module = await import('data:text/javascript;base64,' + Buffer.from(source).toString('base64') + '#' + instance++);
    module.register((name, handler) => {
        handlers.set(name, handler);
        return { catch() {} };
    });
    const emit = (name, event, next = async () => ({ preserved: true })) => handlers.get(name)($, event, next);
    return {
        events, operations, scheduled, emit,
        set_usage: value => { usage = value; },
        set_environment: value => { environment = value; },
        set_transport: value => { transport = value; },
        start: async (id, source = 'resume', next) => {
            session_id = id;
            return emit('classic.SessionStart', { source, session_id: id }, next);
        },
        resume: async id => {
            const result = await emit('classic.SessionStart', { source: 'resume', session_id: id });
            session_id = id;
            return result;
        },
        end: id => emit('session.end', { sessionId: id }),
    };
}

// Public Mods contract: session.start runs when the module loads, while native
// clear/resume emits session.end and classic.SessionStart. Reopening the same
// transcript is a new activation, even when its raw session ID equals an old A.
const completed_start_listeners = [];
for (const [index, source] of sources.entries()) {
    const test = await fixture(source);
    const entered = deferred();
    const native_start = deferred();
    const early_binding = deferred();
    let start_completed = false;
    test.set_transport(async operation => {
        if (operation === 'binding') {
            // Directory allocation precedes accepted registration publication.
            // Hold an early subprocess failure until after native completion.
            if (!start_completed) return early_binding.promise;
            return { provider_session_id: 'A', transition_nonce: 'b'.repeat(32) };
        }
        if (operation === 'ready') return { completed: true };
        throw new Error('fixture current listener stopped');
    });
    const starting = test.start('A', 'startup', async () => {
        entered.resolve();
        return native_start.promise;
    });
    await entered.promise;
    await test.emit('session.start', { isInteractive: true });
    const early_listeners = test.scheduled.splice(0).map(callback => callback());
    native_start.resolve({ preserved: true });
    assert.deepEqual(await starting, { preserved: true });
    start_completed = true;
    if (early_listeners.length) early_binding.reject(new Error('registration was not published yet'));
    await Promise.all(early_listeners);
    completed_start_listeners.push(test.scheduled.length);
    if (test.scheduled.length !== 1) continue;
    test.operations.length = 0;
    await test.scheduled[0]();
    assert.deepEqual(test.operations.map(item => item.operation), index === 0
        ? ['binding', 'listen'] : ['binding', 'ready', 'receive']);
}
assert.deepEqual(completed_start_listeners, [1, 1],
    'admission and dispatcher native completion own listeners after bootstrap and late binding failure');

for (const [index, source] of sources.entries()) {
    const test = await fixture(source);
    await test.emit('session.start', { isInteractive: true });
    await test.start('A', 'startup');
    await test.end('A');
    await test.start('B', 'clear');
    await test.end('B');
    const scheduled = test.scheduled.length;
    test.events.length = 0;
    assert.deepEqual(await test.resume('A'), { preserved: true });
    assert.equal(test.scheduled.length, scheduled + 1,
        'accepted native resume starts a listener before session.id switches after the hook');
    assert.deepEqual(test.events, [], 'the departing native session cannot supply returning-A context');
    for (const callback of test.scheduled.slice(0, -1)) await callback();
    assert.deepEqual(test.operations, [], 'retired listeners remain fenced during delayed native identity switching');
    await test.scheduled.at(-1)();
    assert.deepEqual(test.operations.map(item => item.operation), index === 0
        ? ['binding', 'listen'] : ['binding', 'ready', 'receive']);
    assert.deepEqual(test.operations[0].value, { session_id: 'A' });
}

for (const source of sources) {
    const test = await fixture(source);
    const pending = deferred();
    test.set_environment(key => key === 'VNM_CLAUDE_WAKE_DIRECTORY' ? pending.promise : undefined);
    const bootstrap = test.emit('session.start', { isInteractive: true });
    await test.start('A', 'startup');
    assert.equal(test.scheduled.length, 0, 'listener waits for the native wake environment');
    pending.resolve('/private/wake');
    await bootstrap;
    assert.equal(test.scheduled.length, 1, 'native startup during environment lookup still starts the current listener');
    await test.scheduled[0]();
    assert.ok(test.operations.length > 0, 'the bootstrapped listener belongs to the current activation');
}

for (const [index, source] of sources.entries()) {
    const test = await fixture(source);
    await test.emit('session.start', { isInteractive: true });
    await test.start('A', 'startup');
    await test.end('A');
    await test.start('B');
    await test.end('B');
    assert.deepEqual(await test.start('A'), { preserved: true });
    assert.equal(test.scheduled.length, 4, 'each native activation restarts without another session.start');
    for (const callback of test.scheduled.slice(0, -1)) await callback();
    assert.deepEqual(test.operations, [], 'retired scheduled listeners cannot act on the returning A');
    await test.scheduled.at(-1)();
    assert.deepEqual(test.operations.map(item => item.operation), index === 0 ? ['binding', 'listen'] : ['binding', 'ready', 'receive']);
    assert.deepEqual(test.operations[0].value, { session_id: 'A' });
    assert.deepEqual(test.operations[1].value, { provider_session_id: 'A', transition_nonce: 'b'.repeat(32) });
    assert.ok(!test.operations.some(item => item.operation === 'close'), 'native helper owns registration cleanup');
}

for (const source of sources) {
    const test = await fixture(source);
    await test.emit('session.start', { isInteractive: true });
    const entered = deferred();
    const pending = deferred();
    test.set_transport(async operation => {
        if (operation === 'binding') return { provider_session_id: 'A', transition_nonce: 'b'.repeat(32) };
        if (operation === 'ready') return { completed: true };
        entered.resolve();
        return pending.promise;
    });
    const old_listener = test.scheduled[0]();
    await entered.promise;
    await test.end('A');
    await test.start('B');
    await test.end('B');
    await test.start('A');
    pending.reject(new Error('late failure from the first A'));
    await old_listener;
    test.operations.length = 0;
    test.set_transport(async operation => {
        if (operation === 'binding') return { provider_session_id: 'A', transition_nonce: 'b'.repeat(32) };
        if (operation === 'ready') return { completed: true };
        throw new Error('fixture current listener stopped');
    });
    await test.scheduled.at(-1)();
    assert.ok(test.operations.length > 0, 'an old asynchronous failure cannot disable the returning A');
}

for (const source of sources) {
    const test = await fixture(source);
    await test.emit('session.start', { isInteractive: true });
    const entered = deferred();
    const pending = deferred();
    const old_start = test.start('A', 'startup', async () => {
        entered.resolve();
        return pending.promise;
    });
    await entered.promise;
    await test.end('A');
    await test.start('B');
    await test.end('B');
    await test.start('A');
    const scheduled = test.scheduled.length;
    test.events.length = 0;
    pending.resolve({ preserved: true });
    assert.deepEqual(await old_start, { preserved: true });
    assert.equal(test.scheduled.length, scheduled, 'an old start cannot restart a successor listener');
    assert.deepEqual(test.events, [], 'an old start cannot publish telemetry for the returning A');
}

{
    const test = await fixture(sources[0]);
    await test.start('A', 'startup');
    const entered = deferred();
    const pending = deferred();
    test.set_usage(async () => {
        test.set_usage(async () => ({ context: { tokens: 20, window: 100 } }));
        entered.resolve();
        return pending.promise;
    });
    const old_completion = test.emit('turn.complete', { reason: 'answer' });
    await entered.promise;
    await test.end('A');
    await test.start('B');
    await test.end('B');
    await test.start('A');
    test.events.length = 0;
    pending.resolve({ context: { tokens: 99, window: 100 } });
    await old_completion;
    assert.deepEqual(test.events, [], 'late usage from the first A cannot overwrite the returning A');
}

for (const source of sources) {
    const test = await fixture(source);
    await test.emit('session.start', { isInteractive: true });
    const next = async () => ({ preserved: true });
    next.signal = { aborted: true };
    await test.start('A', 'startup', next);
    assert.equal(test.scheduled.length, 1, 'abandoned native events cannot start a new listener');
    assert.deepEqual(test.events, [], 'abandoned native events cannot publish new telemetry');
}

console.log('Claude activation hook fixture checks passed.');
