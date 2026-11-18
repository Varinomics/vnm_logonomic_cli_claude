import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';

const source = await readFile(process.argv[2], 'utf8');
let instance = 0;

function deferred() {
    let resolve;
    const promise = new Promise(yes => { resolve = yes; });
    return { promise, resolve };
}

// The host's model ledger for one accepted activation (SDK apply_model_update):
// each actor admits only a strictly greater sequence of that same activation.
function host_ledger(provider_session_id, transition_nonce) {
    const actors = new Map();
    return {
        admit: report => {
            for (const update of report.model_state.updates) {
                const held = actors.get(update.agent_id);
                if (update.provider_session_id !== provider_session_id ||
                    update.transition_nonce !== transition_nonce ||
                    (held && BigInt(update.sequence) <= held.sequence))
                {
                    continue;
                }
                actors.set(update.agent_id, { sequence: BigInt(update.sequence), model: update.model });
            }
        },
        model: agent_id => actors.get(agent_id)?.model,
    };
}

async function fixture({ binding = true, relay } = {}) {
    let handlers;
    const scheduled = [];
    const reports = [];
    let session_id = 'A';
    let nonce = 'a'.repeat(32);
    let roster = async () => [{ id: 'child-1' }, { id: 'child-2' }];
    let binding_hold;
    // Native session state outlives a module reload; /clear, /resume and
    // /branch reset it. A write given ifVersion is compare-and-set. As in the
    // engine, a reset raises a version floor, so later writes stand above every
    // version written before it.
    let state = new Map();
    let version_floor = 0;
    const $ = {
        session: { id: async () => session_id, usage: async () => ({}) },
        agent: { list: () => roster() },
        env: { get: async key => key === 'VNM_CLAUDE_WAKE_DIRECTORY' ? '/private/wake' : undefined },
        plugin: { root: '/plugin' },
        clock: { after: (delay, callback) => scheduled.push(callback) },
        state: {
            get: async ref => {
                const held = state.get(ref.plugin + '/' + ref.key);
                return { value: held && structuredClone(held.value), version: held?.version ?? 0 };
            },
            set: async (ref, value, options) => {
                assert.equal(ref.plugin, 'logonomic-claude-hooks', 'a plugin writes only its own state');
                const key = ref.plugin + '/' + ref.key;
                const version = state.get(key)?.version ?? 0;
                if (options?.ifVersion !== undefined && options.ifVersion !== version) {
                    return { isSet: false, version };
                }
                const written = Math.max(version, version_floor) + 1;
                state.set(key, { value: structuredClone(value), version: written });
                return { isSet: true, version: written };
            },
        },
        process: { run: async (argv, options) => {
            if (argv[1].endsWith('/logonomic-hook-relay.cjs')) {
                const report = JSON.parse(options.stdin);
                if (report.hook_event_name === 'ModModel') {
                    reports.push(report);
                    if (relay) await relay();
                }
                return { exitCode: 0, stdout: '', stderr: '' };
            }
            if (argv[2] === 'binding') {
                const hold = binding_hold;
                binding_hold = undefined;
                if (hold) await hold;
                return {
                    exitCode: 0, stdout: JSON.stringify({ provider_session_id: session_id, transition_nonce: nonce }),
                };
            }
            throw new Error('fixture listener stopped after source binding');
        } },
    };
    // A module reload evaluates a new copy. Native events reach only that copy;
    // the old copy's in-flight hooks settle on their own.
    const load = async () => {
        const registered = new Map();
        const module = await import('data:text/javascript;base64,' + Buffer.from(source).toString('base64') + '#' + instance++);
        module.register((name, handler) => { registered.set(name, handler); return { catch() {} }; });
        handlers = registered;
    };
    const emit = (name, event, next = async () => ({ preserved: true })) => handlers.get(name)($, event, next);
    const bind = () => scheduled.shift()();
    const flush = async () => {
        while (scheduled.length) await scheduled.shift()();
    };
    await load();
    await emit('session.start', { isInteractive: true });
    if (binding) await bind();
    return {
        reports, emit, bind, flush,
        set_roster: callback => { roster = callback; },
        // The next binding read waits until the returned hold resolves.
        hold_binding: () => {
            const hold = deferred();
            binding_hold = hold.promise;
            return hold;
        },
        // The native session, its accepted registration and nonce are unchanged.
        reload: async () => {
            await load();
            await emit('session.start', { isInteractive: true });
            await bind();
        },
        activate: async (id, value) => {
            await emit('session.end', { sessionId: session_id });
            for (const held of state.values()) version_floor = Math.max(version_floor, held.version);
            state = new Map();
            session_id = id;
            nonce = value;
            await emit('classic.SessionStart', { session_id: id, source: 'resume' });
            await bind();
        },
    };
}

function response(model, stopReason = 'end_turn') {
    return { turnId: 'turn', index: 0, answer: 'unchanged', toolUses: [], stopReason,
        usage: model === null ? null : { model, input_tokens: 1, output_tokens: 1 } };
}

// `entered` resolves when the request reaches the native model, after the
// source has captured its binding and sequence.
function start_step(test, event, result, pending, aborted = false) {
    const entered = deferred();
    const next = async function* () {
        entered.resolve();
        yield { kind: 'text', index: 0, text: result.answer };
        if (pending) await pending;
        return result;
    };
    next.signal = { aborted };
    const iterator = test.emit('turn.step', { turnId: 'turn', index: 0, model: 'launcher-selection', ...event }, next);
    const done = (async () => {
        assert.deepEqual(await iterator.next(), { done: false, value: { kind: 'text', index: 0, text: result.answer } });
        const returned = await iterator.next();
        assert.equal(returned.done, true);
        assert.equal(returned.value, result, 'the native response object passes through untouched');
    })();
    return { entered: entered.promise, native_done: done, done: done.then(test.flush) };
}

async function step(test, event, result, pending, aborted = false) {
    await start_step(test, event, result, pending, aborted).done;
}

// Oracle: a step's usage.model is the API-reported model of that one response.
// event.model and aggregate session usage are not serving-model evidence.
{
    const held = deferred();
    const test = await fixture({ relay: () => held.promise });
    let complete = false;
    const started = start_step(test, {}, response('served'));
    const native = started.native_done.then(() => { complete = true; });
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(complete, true, 'a held reporting relay cannot delay the native result');
    assert.equal(test.reports.length, 1, 'the reporting relay is in flight');
    const slow = deferred();
    const older = start_step(test, {}, response('older'), slow.promise);
    await older.entered;
    await step(test, {}, response('latest'));
    slow.resolve();
    await older.done;
    assert.equal(test.reports.length, 1, 'later results do not start competing relays');
    held.resolve();
    await Promise.all([native, started.done]);
    assert.deepEqual(test.reports.map(report => report.model_state.updates[0].model), ['served', 'latest']);
}

{
    const test = await fixture();
    await step(test, {}, response('root-served'));
    await step(test, { agentId: 'child-1' }, response('child-one-served'));
    await step(test, { agentId: 'child-2' }, response('child-two-served'));
    assert.deepEqual(test.reports.map(report => report.model_state.updates[0]), [
        { provider_session_id: 'A', transition_nonce: 'a'.repeat(32), sequence: '1', agent_id: '',
            state: 'confirmed', model: 'root-served', source: 'claude.turn.step.usage' },
        { provider_session_id: 'A', transition_nonce: 'a'.repeat(32), sequence: '2', agent_id: 'child-1',
            state: 'confirmed', model: 'child-one-served', source: 'claude.turn.step.usage' },
        { provider_session_id: 'A', transition_nonce: 'a'.repeat(32), sequence: '3', agent_id: 'child-2',
            state: 'confirmed', model: 'child-two-served', source: 'claude.turn.step.usage' },
    ]);
    await step(test, { agentId: 'internal-memory' }, response('internal'));
    await step(test, { agentId: 'child-1' }, response(null));
    await step(test, {}, response('<synthetic>'));
    await step(test, {}, response('compaction-model', 'compaction'));
    await step(test, {}, response('aborted-model'), undefined, true);
    assert.equal(test.reports.length, 3, 'internal, unavailable, synthetic and aborted evidence cannot relabel an actor');
    const preserved = { native: 'switch' };
    assert.equal(await test.emit('classic.PostModelSwitch', {
        session_id: 'A', from_model: 'root-served', to_model: 'resolved-switch', requested_model: 'opus',
    }, async () => preserved), preserved);
    await test.flush();
    assert.equal(test.reports.at(-1).model_state.updates[0].model, 'resolved-switch');
    assert.equal(test.reports.at(-1).model_state.updates[0].source, 'claude.PostModelSwitch');
}

{
    const test = await fixture();
    const pending = deferred();
    const old = start_step(test, {}, response('old-slow-response'), pending.promise);
    await old.entered;
    await step(test, {}, response('new-response'));
    pending.resolve();
    await old.done;
    assert.deepEqual(test.reports.map(report => report.model_state.updates[0].sequence), ['2', '1'],
        'completion order cannot rewrite the source sequence; the host rejects stale order');
}

{
    const test = await fixture();
    const pending = deferred();
    const old = start_step(test, {}, response('first-A'), pending.promise);
    await old.entered;
    await test.activate('B', 'b'.repeat(32));
    await test.activate('A', 'c'.repeat(32));
    pending.resolve();
    await old.done;
    assert.deepEqual(test.reports, [], 'A to B to A cannot borrow the returning activation');
    await step(test, {}, response('returning-A'));
    assert.equal(test.reports[0].model_state.updates[0].transition_nonce, 'c'.repeat(32));
    assert.equal(test.reports[0].model_state.updates[0].sequence, '1');
}

{
    const test = await fixture({ binding: false });
    const pending = deferred();
    const early = start_step(test, {}, response('before-binding'), pending.promise);
    await early.entered;
    await test.bind();
    pending.resolve();
    await early.done;
    assert.deepEqual(test.reports, [], 'binding obtained later cannot be stamped onto an earlier observation');
}

// Oracle: a native module reload evaluates a new copy of this module while the
// session, its accepted registration and transition nonce stay unchanged. The
// host admits only a sequence above that activation's accepted barrier, so a
// changed model after the reload is admitted, and a request captured before
// the reload cannot replace it when it completes later.
{
    const test = await fixture();
    const host = host_ledger('A', 'a'.repeat(32));
    for (let index = 0; index < 20; ++index) {
        await step(test, {}, response('before-reload'));
    }
    const pending = deferred();
    const old = start_step(test, {}, response('captured-before-reload'), pending.promise);
    await old.entered;
    await test.reload();
    await step(test, {}, response('after-reload'));
    const reloaded = test.reports.at(-1).model_state.updates[0];
    assert.equal(reloaded.transition_nonce, 'a'.repeat(32), 'the reload keeps the accepted activation');
    test.reports.forEach(host.admit);
    assert.equal(host.model(''), 'after-reload',
        `a reloaded source continues above the accepted sequence (reported ${reloaded.sequence})`);
    const admitted = test.reports.length;
    pending.resolve();
    await old.done;
    const late = test.reports.slice(admitted);
    assert.ok(late.every(report => report.model_state.updates[0].transition_nonce === 'a'.repeat(32)),
        'the overlapping completion keeps the identity it captured before the reload');
    late.forEach(host.admit);
    assert.equal(host.model(''), 'after-reload', 'an earlier capture cannot roll back the later admitted model');
}

// Captures still running in the replaced copy cannot be published above the
// sequences of the copy that replaced it, whichever actor they belong to.
{
    const test = await fixture();
    const host = host_ledger('A', 'a'.repeat(32));
    await step(test, {}, response('before-reload'));
    const child_pending = deferred();
    const root_pending = deferred();
    const child = start_step(test, { agentId: 'child-1' }, response('child-before-reload'), child_pending.promise);
    await child.entered;
    const root = start_step(test, {}, response('root-before-reload'), root_pending.promise);
    await root.entered;
    await test.reload();
    await test.emit('classic.PostModelSwitch', { session_id: 'A', to_model: 'switched-after-reload' },
        async () => ({ preserved: true }));
    await test.flush();
    child_pending.resolve();
    root_pending.resolve();
    await child.done;
    await root.done;
    test.reports.forEach(host.admit);
    assert.equal(host.model(''), 'switched-after-reload', 'a replaced copy cannot publish above its successor');
}

// Oracle: after a reload, native events reach only the new copy, while work the
// replaced copy already started runs on until its in-flight hooks settle; a
// listener started by its `after` timer may still be awaiting its binding.
// That binding completing later cannot take model sequencing from the copy that
// replaced it: the new copy's own in-flight request and its later changed model
// stay publishable under the unchanged activation.
{
    const test = await fixture({ binding: false });
    const host = host_ledger('A', 'a'.repeat(32));
    const old_binding = test.hold_binding();
    const old_listener = test.bind();
    const old_pending = deferred();
    const old_step = start_step(test, {}, response('replaced-copy-in-flight'), old_pending.promise);
    await old_step.entered;
    await test.reload();
    await step(test, {}, response('before-late-binding'));
    const new_pending = deferred();
    const new_step = start_step(test, {}, response('captured-before-late-binding'), new_pending.promise);
    await new_step.entered;
    old_binding.resolve();
    await old_listener;
    old_pending.resolve();
    await old_step.done;
    new_pending.resolve();
    await new_step.done;
    await step(test, {}, response('changed-after-late-binding'));
    test.reports.forEach(host.admit);
    const published = test.reports.map(report => report.model_state.updates[0]);
    assert.ok(published.every(update => update.transition_nonce === 'a'.repeat(32)));
    assert.equal(host.model(''), 'changed-after-late-binding',
        `a replaced copy's late binding cannot silence its successor (published: ${
            published.map(update => update.model + '@' + update.sequence).join(', ')})`);
    assert.deepEqual(published.map(update => update.model),
        ['before-late-binding', 'captured-before-late-binding', 'changed-after-late-binding']);
}

// A reload continues above the highest published sequence even when an
// earlier capture completed after a later one.
{
    const test = await fixture();
    const host = host_ledger('A', 'a'.repeat(32));
    const pending = deferred();
    const slow = start_step(test, {}, response('slow-before-reload'), pending.promise);
    await slow.entered;
    await step(test, {}, response('fast-before-reload'));
    pending.resolve();
    await slow.done;
    await test.reload();
    await step(test, {}, response('after-reload'));
    test.reports.forEach(host.admit);
    assert.equal(host.model(''), 'after-reload', 'completion order cannot lower the next claim');
}

{
    const test = await fixture();
    const pending = deferred();
    const old = start_step(test, {}, response('captured-before-reload'), pending.promise);
    await old.entered;
    await test.reload();
    await test.activate('B', 'b'.repeat(32));
    await test.activate('A', 'c'.repeat(32));
    await step(test, {}, response('returning-A'));
    pending.resolve();
    await old.done;
    const host = host_ledger('A', 'c'.repeat(32));
    test.reports.forEach(host.admit);
    assert.ok(test.reports
        .filter(report => report.model_state.updates[0].model === 'captured-before-reload')
        .every(report => report.model_state.updates[0].transition_nonce === 'a'.repeat(32)),
    'a capture from before the reload cannot rebind to a later activation');
    assert.equal(host.model(''), 'returning-A');
}

{
    const test = await fixture();
    const entered = deferred();
    const pending = deferred();
    test.set_roster(async () => { entered.resolve(); return pending.promise; });
    const old = step(test, { agentId: 'child-1' }, response('old-child'));
    await entered.promise;
    await test.activate('B', 'b'.repeat(32));
    pending.resolve([{ id: 'child-1' }]);
    await old;
    assert.deepEqual(test.reports, [], 'a delayed roster read cannot publish into another activation');
}

console.log('Claude model hook attribution checks passed.');
