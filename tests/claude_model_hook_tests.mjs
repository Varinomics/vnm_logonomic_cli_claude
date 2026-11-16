import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';

const source = await readFile(process.argv[2], 'utf8');
let instance = 0;

function deferred() {
    let resolve;
    const promise = new Promise(yes => { resolve = yes; });
    return { promise, resolve };
}

async function fixture({ binding = true } = {}) {
    const handlers = new Map();
    const scheduled = [];
    const reports = [];
    let session_id = 'A';
    let nonce = 'a'.repeat(32);
    let roster = async () => [{ id: 'child-1' }, { id: 'child-2' }];
    const $ = {
        session: { id: async () => session_id, usage: async () => ({}) },
        agent: { list: () => roster() },
        env: { get: async key => key === 'VNM_CLAUDE_WAKE_DIRECTORY' ? '/private/wake' : undefined },
        plugin: { root: '/plugin' },
        clock: { after: (delay, callback) => scheduled.push(callback) },
        process: { run: async (argv, options) => {
            if (argv[1].endsWith('/logonomic-hook-relay.cjs')) {
                const report = JSON.parse(options.stdin);
                if (report.hook_event_name === 'ModModel') reports.push(report);
                return { exitCode: 0, stdout: '', stderr: '' };
            }
            if (argv[2] === 'binding') return {
                exitCode: 0, stdout: JSON.stringify({ provider_session_id: session_id, transition_nonce: nonce }),
            };
            throw new Error('fixture listener stopped after source binding');
        } },
    };
    const module = await import('data:text/javascript;base64,' + Buffer.from(source).toString('base64') + '#' + instance++);
    module.register((name, handler) => { handlers.set(name, handler); return { catch() {} }; });
    const emit = (name, event, next = async () => ({ preserved: true })) => handlers.get(name)($, event, next);
    const bind = () => scheduled.at(-1)();
    await emit('session.start', { isInteractive: true });
    if (binding) await bind();
    return {
        reports, emit, bind,
        set_roster: callback => { roster = callback; },
        activate: async (id, value) => {
            await emit('session.end', { sessionId: session_id });
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

async function step(test, event, result, pending, aborted = false) {
    const next = async function* () {
        yield { kind: 'text', index: 0, text: result.answer };
        if (pending) await pending;
        return result;
    };
    next.signal = { aborted };
    const iterator = test.emit('turn.step', { turnId: 'turn', index: 0, model: 'launcher-selection', ...event }, next);
    assert.deepEqual(await iterator.next(), { done: false, value: { kind: 'text', index: 0, text: result.answer } });
    const returned = await iterator.next();
    assert.equal(returned.done, true);
    assert.equal(returned.value, result, 'the native response object passes through untouched');
}

// Oracle: a step's usage.model is the API-reported model of that one response.
// event.model and aggregate session usage are not serving-model evidence.
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
    assert.equal(test.reports.at(-1).model_state.updates[0].model, 'resolved-switch');
    assert.equal(test.reports.at(-1).model_state.updates[0].source, 'claude.PostModelSwitch');
}

{
    const test = await fixture();
    const pending = deferred();
    const old = step(test, {}, response('old-slow-response'), pending.promise);
    await Promise.resolve();
    await step(test, {}, response('new-response'));
    pending.resolve();
    await old;
    assert.deepEqual(test.reports.map(report => report.model_state.updates[0].sequence), ['2', '1'],
        'completion order cannot rewrite the source sequence; the host rejects stale order');
}

{
    const test = await fixture();
    const pending = deferred();
    const old = step(test, {}, response('first-A'), pending.promise);
    await Promise.resolve();
    await test.activate('B', 'b'.repeat(32));
    await test.activate('A', 'c'.repeat(32));
    pending.resolve();
    await old;
    assert.deepEqual(test.reports, [], 'A to B to A cannot borrow the returning activation');
    await step(test, {}, response('returning-A'));
    assert.equal(test.reports[0].model_state.updates[0].transition_nonce, 'c'.repeat(32));
    assert.equal(test.reports[0].model_state.updates[0].sequence, '1');
}

{
    const test = await fixture({ binding: false });
    const pending = deferred();
    const early = step(test, {}, response('before-binding'), pending.promise);
    await Promise.resolve();
    await test.bind();
    pending.resolve();
    await early;
    assert.deepEqual(test.reports, [], 'binding obtained later cannot be stamped onto an earlier observation');
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
