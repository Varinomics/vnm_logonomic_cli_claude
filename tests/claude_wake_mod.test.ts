import { expect, mock, test } from 'claude-code/testing';

// These run in Claude's actual Mod engine, with model/network/filesystem calls
// stubbed. They qualify native origin and hook dispatch, not a live TUI turn.
const cases = [
    { name: 'idle answer submits with plugin origin', expected: 'accepted' },
    { name: 'user draft is preserved', draft: 'unfinished text', reason: 'claude_prompt_draft' },
    { name: 'busy root turn is deferred', busy: true, reason: 'claude_turn_busy' },
    { name: 'interrupted turn stays paused', aborted: true, reason: 'claude_session_paused' },
    { name: 'subagent completion does not publish root readiness', subagent: true, reason: 'claude_session_paused' },
    { name: 'unknown initial state stays paused', initial: true, reason: 'claude_session_paused' },
    { name: 'unknown peer policy is refused', inbound: null, reason: 'claude_inbound_policy_unknown' },
    { name: 'native hold is respected', inbound: 'hold', reason: 'claude_inbound_held' },
    { name: 'native refuse is respected', inbound: 'refuse', reason: 'claude_inbound_refused' },
    { name: 'project hold overrides flag accept', project: 'hold', reason: 'claude_inbound_held' },
    { name: 'local refuse overrides flag accept', local: 'refuse', reason: 'claude_inbound_refused' },
    { name: 'project accept cannot approve unknown policy', inbound: null, project: 'accept', reason: 'claude_inbound_policy_unknown' },
    { name: 'changed native identity is refused', changed: true, reason: 'claude_native_session_changed' },
    { name: 'completion cannot signal a replacement session', interleaved: true, reason: 'claude_native_session_changed' },
    { name: 'expired delivery does not submit', expired: true, reason: 'claude_wake_expired' },
    { name: 'duplicate delivery reuses receipt', duplicate: true, expected: 'accepted' },
    { name: 'policy change before submission is checked again', race: true, reason: 'claude_native_prompt_declined' },
    { name: 'missing final guard evidence fails closed', guard_failure: true, reason: 'claude_native_prompt_declined' },
];

for (const item of cases) {
    test(item.name, async ($, on) => {
        const engine = $;
        const clock = mock.clock(on, { now: 1000 });
        mock.env(on, { VNM_CLAUDE_WAKE_DIRECTORY: '/private/wake/test' });
        let identity_calls = 0;
        let submitted = 0;
        let reserved = 0;
        let listened = false;
        let completed = 0;
        let interleaved = false;
        let reply;
        const request = {
            id: 'a'.repeat(32), operation: 'notify', expires_at_ms: item.expired ? 999 : 10000,
            payload: { session: { provider_session_id: 'native-session' }, correlation: { transition_nonce: 'b'.repeat(32) },
                notification_id: 'notice-1', instruction: 'Read the Logonomic mailbox.' },
        };
        on('session.id', () => ({ value: interleaved || item.changed && identity_calls++ > 0 ? 'different-session' : 'native-session' }));
        on('session.usage', () => ({ value: { context: { tokens: 64000, window: 200000 } } }));
        on('settings.read', ($, event) => ({ value: {
            crossSessionInbound: event.source === 'project' ? item.project : event.source === 'local' ? item.local :
                event.source === 'flag' ? (item.race && reserved ? 'hold' : item.inbound === null ? undefined : item.inbound ?? 'accept') : undefined,
        } }));
        on('prompt.read', () => ({ value: { text: item.draft ?? '', cursor: item.draft?.length ?? 0 } }));
        on('session.start', () => ({ cwd: '/work' }));
        on('turn.start', ($, event) => ({ turnId: event.turnId }));
        on('turn.complete', async () => {
            if (item.interleaved) {
                interleaved = true;
                await engine.session.start({ cwd: '/work', surface: 'terminal', isInteractive: true });
            }
            return { text: 'answer' };
        });
        on('process.run', async ($, event) => {
            if (event.argv[1].endsWith('/scripts/logonomic-hook-relay.cjs')) {
                const report = JSON.parse(event.init.stdin);
                expect(report).toEqual({
                    hook_event_name: item.aborted ? 'ModContext' : 'ModTurnComplete',
                    session_id: 'native-session', reason: item.aborted ? 'aborted' : 'answer',
                    context_state: { used_tokens: 64000, context_window: 200000 },
                });
                if (report.hook_event_name === 'ModTurnComplete') completed++;
                return { value: { exitCode: 0, stdout: '', stderr: '' } };
            }
            const operation = event.argv[2];
            let result;
            if (operation === 'binding') result = { provider_session_id: 'native-session', transition_nonce: 'b'.repeat(32) };
            else if (operation === 'listen') {
                if (listened) return { deny: 'test-completed' };
                listened = true;
                result = request;
            }
            else if (operation === 'inspect-dispatch') result = { available: true };
            else if (operation === 'guard-request') {
                if (item.guard_failure) throw new Error('guard_evidence_missing');
                result = request;
            }
            else if (operation === 'submit') {
                // The actual dispatcher is separately qualified in a live TUI.
                // Here native dispatch runs the guard with that plugin's origin.
                result = await engine.prompt.submit({ text: request.payload.instruction, wait: false,
                    origin: { kind: 'plugin', name: 'logonomic-wake-dispatch' } });
            }
            else if (operation === 'reserve') {
                reserved++;
                result = item.duplicate ? { disposition: 'accepted', receipt: 'notice-1' } : { reserved: true };
            }
            else if (operation === 'reply') {
                reply = JSON.parse(event.init.stdin);
                result = { completed: true };
            }
            else throw new Error('Unexpected bridge operation');
            return { value: { exitCode: 0, stdout: JSON.stringify(result), stderr: '' } };
        });
        on('prompt.submit', ($, event) => {
            submitted++;
            expect(event.origin).toEqual({ kind: 'plugin', name: 'logonomic-wake-dispatch' });
            expect(event.text).toBe('Read the Logonomic mailbox.');
            return { text: event.text, origin: event.origin };
        });
        await $.session.start({ cwd: '/work', surface: 'terminal', isInteractive: true });
        if (!item.initial) await $.turn.complete({ answer: 'done', durationMs: 1, turnId: 'turn', isAborted: !!item.aborted,
            reason: item.aborted ? 'aborted' : 'answer', ...(item.subagent ? { agentId: 'child' } : {}) });
        if (item.busy) await $.turn.start({ turnId: 'busy' });
        await clock.settle();
        expect(completed).toBe(item.initial || item.aborted || item.subagent || item.changed || item.interleaved ? 0 : 1);
        if (item.reason) {
            expect(reply.reason).toBe(item.reason);
            expect(submitted).toBe(0);
        }
        else {
            expect(reply).toEqual({ disposition: 'accepted', receipt: 'notice-1' });
            expect(submitted).toBe(item.duplicate ? 0 : 1);
        }
    });
}

for (const mode of ['anchored', 'unavailable', 'missing', 'subagent']) {
    test('classic start preserves lifecycle with owner ' + mode, async ($, on) => {
        mock.env(on, mode === 'missing' ? {} : { VNM_CLAUDE_WAKE_OWNER_HELPER: '/private/helpers/claude_helper' });
        const calls = [];
        on('process.run', ($, event) => {
            expect(event.argv).toEqual(['/private/helpers/claude_helper', 'wake-owner']);
            calls.push('anchor');
            if (mode === 'unavailable') return { deny: 'owner_unavailable' };
            return { value: { exitCode: 0, stdout: '', stderr: '' } };
        });
        on('classic.SessionStart', () => {
            calls.push('classic');
            return { additionalContext: ['Ordinary session context.'] };
        });
        const result = await $.classic.SessionStart({ source: 'startup', ...(mode === 'subagent' ? { agent_id: 'child' } : {}) });
        expect(calls).toEqual(mode === 'missing' || mode === 'subagent' ? ['classic'] : ['anchor', 'classic']);
        expect(result).toEqual({ additionalContext: ['Ordinary session context.'] });
    });
}
