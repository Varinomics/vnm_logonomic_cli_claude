import { expect, mock, test } from 'claude-code/testing';

// The actual native Mod engine dispatches the production module. Native model,
// roster and subprocess responses are scripted; this makes no account calls.
for (const item of [
    { name: 'root response', model: 'root-served', expected: true },
    { name: 'own child response', agentId: 'child', model: 'child-served', expected: true },
    { name: 'internal fork excluded', agentId: 'memory-helper', model: 'memory-model', expected: false },
    { name: 'no usage excluded', model: null, expected: false },
    { name: 'compaction response excluded', model: 'compact-model', stopReason: 'compaction', expected: false },
]) {
    test('model evidence: ' + item.name, async ($, on) => {
        const clock = mock.clock(on, { now: 1000 });
        mock.env(on, { VNM_CLAUDE_WAKE_DIRECTORY: '/private/wake/test' });
        const reports = [];
        on('session.id', () => ({ value: 'native-session' }));
        on('session.start', () => ({ cwd: '/work' }));
        on('agent.list', () => ({ value: [{ id: 'child', kind: 'local_agent', name: 'Worker' }] }));
        on('process.run', ($, event) => {
            if (event.argv[1].endsWith('/logonomic-hook-relay.cjs')) {
                reports.push(JSON.parse(event.init.stdin));
                return { value: { exitCode: 0, stdout: '', stderr: '' } };
            }
            if (event.argv[2] === 'binding') return { value: { exitCode: 0,
                stdout: JSON.stringify({ provider_session_id: 'native-session', transition_nonce: 'b'.repeat(32) }),
                stderr: '' } };
            return { deny: 'fixture-listener-stopped' };
        });
        const native = { turnId: 'turn', index: 0, answer: 'unchanged', toolUses: [],
            stopReason: item.stopReason ?? 'end_turn', usage: item.model === null ? null : {
                model: item.model, input_tokens: 1, output_tokens: 1,
                cache_read_input_tokens: 0, cache_creation_input_tokens: 0,
            } };
        on('turn.step', async function* () {
            yield { kind: 'text', index: 0, text: 'unchanged' };
            return native;
        });
        await $.session.start({ cwd: '/work', surface: 'terminal', isInteractive: true });
        await clock.settle();
        const stream = $.turn.step({ turnId: 'turn', index: 0, messageCount: 1,
            model: 'launcher-only', ...(item.agentId ? { agentId: item.agentId } : {}) });
        const chunks = [];
        let part;
        do {
            part = await stream.next();
            if (!part.done) chunks.push(part.value);
        } while (!part.done);
        expect(part.value).toEqual(native);
        expect(chunks).toEqual([{ kind: 'text', index: 0, text: 'unchanged' }]);
        expect(reports.length).toBe(item.expected ? 1 : 0);
        if (item.expected) expect(reports[0]).toEqual({ hook_event_name: 'ModModel',
            session_id: 'native-session', agent_id: item.agentId ?? '', model_state: { version: 1, updates: [{
                provider_session_id: 'native-session', transition_nonce: 'b'.repeat(32), sequence: '1',
                agent_id: item.agentId ?? '', state: 'confirmed', model: item.model,
                source: 'claude.turn.step.usage',
            }] } });
    });
}

test('model evidence: resolved native session switch', async ($, on) => {
    const clock = mock.clock(on, { now: 1000 });
    mock.env(on, { VNM_CLAUDE_WAKE_DIRECTORY: '/private/wake/test' });
    const reports = [];
    on('session.id', () => ({ value: 'native-session' }));
    on('session.start', () => ({ cwd: '/work' }));
    on('process.run', ($, event) => {
        if (event.argv[1].endsWith('/logonomic-hook-relay.cjs')) {
            reports.push(JSON.parse(event.init.stdin));
            return { value: { exitCode: 0, stdout: '', stderr: '' } };
        }
        if (event.argv[2] === 'binding') return { value: { exitCode: 0,
            stdout: JSON.stringify({ provider_session_id: 'native-session', transition_nonce: 'b'.repeat(32) }),
            stderr: '' } };
        return { deny: 'fixture-listener-stopped' };
    });
    on('classic.PostModelSwitch', () => ({ additionalContext: ['native-switch-unchanged'] }));
    await $.session.start({ cwd: '/work', surface: 'terminal', isInteractive: true });
    await clock.settle();
    expect(await $.classic.PostModelSwitch({ session_id: 'native-session',
        from_model: 'before', to_model: 'native-resolved', requested_model: 'opus', source: 'command',
    })).toEqual({ additionalContext: ['native-switch-unchanged'] });
    expect(reports).toEqual([{ hook_event_name: 'ModModel', session_id: 'native-session', agent_id: '',
        model_state: { version: 1, updates: [{ provider_session_id: 'native-session',
            transition_nonce: 'b'.repeat(32), sequence: '1', agent_id: '', state: 'confirmed',
            model: 'native-resolved', source: 'claude.PostModelSwitch',
        }] } }]);
});
