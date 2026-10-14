// Only public Claude Mods APIs are used here. Claude Code owns submission,
// attribution, permission prompts and cancellation of the running model turn.
let active = false;
let busy = false;
let paused = true;
let directory;
let native_id;

async function bridge($, operation, id, value) {
    const result = await $.process.run(
        ['node', `${$.plugin.root}/scripts/logonomic-wake.cjs`, operation, directory, ...(id ? [id] : [])],
        { stdin: value === undefined ? '' : JSON.stringify(value), timeoutMs: 25000 });
    if (result.exitCode !== 0) throw new Error('bridge_unavailable');
    return JSON.parse(result.stdout);
}

async function admission($, request) {
    if (!active || await $.session.id() !== native_id ||
        request.payload.session.provider_session_id !== native_id)
    {
        return { disposition: 'unavailable', reason: 'claude_native_session_changed' };
    }
    if (request.expires_at_ms <= await $.clock.now()) {
        return { disposition: 'deferred', reason: 'claude_wake_expired' };
    }
    // Read live native settings, including the stricter project/local override.
    // Without explicit admission the sender's permission class is unknown.
    const project = await $.settings.read({ source: 'project' });
    const local   = await $.settings.read({ source: 'local' });
    // Project/local accept cannot loosen a missing user/flag/policy setting.
    const policy = await $.settings.read({ source: 'policy' });
    const flag   = await $.settings.read({ source: 'flag' });
    const user   = await $.settings.read({ source: 'user' });
    const inbound = policy.crossSessionInbound ?? flag.crossSessionInbound ?? user.crossSessionInbound;
    const values = [project.crossSessionInbound, local.crossSessionInbound, inbound];
    if (values.includes('refuse')) return { disposition: 'unavailable', reason: 'claude_inbound_refused' };
    if (values.includes('hold')) return { disposition: 'deferred', reason: 'claude_inbound_held' };
    if (values.some(value => value !== undefined && value !== 'accept')) {
        return { disposition: 'unavailable', reason: 'claude_inbound_policy_unknown' };
    }
    if (inbound !== 'accept') return { disposition: 'unavailable', reason: 'claude_inbound_policy_unknown' };
    if (busy) return { disposition: 'deferred', reason: 'claude_turn_busy' };
    if (paused) return { disposition: 'deferred', reason: 'claude_session_paused' };
    if ((await $.prompt.read()).text !== '') return { disposition: 'deferred', reason: 'claude_prompt_draft' };
    return null;
}

async function process_request($, request) {
    const refused = await admission($, request);
    const dispatch = refused ? null : await bridge($, 'inspect-dispatch', undefined, { session_id: native_id });
    const unavailable = refused ?? (dispatch.available ? null : { disposition: 'unavailable', reason: 'claude_dispatch_not_ready' });
    if (request.operation === 'inspect') {
        return unavailable ? { available: false, reason: unavailable.reason } : { available: true };
    }
    if (request.operation !== 'notify') return { disposition: 'unavailable', reason: 'claude_wake_operation_unknown' };
    if (unavailable) return unavailable;
    const reserved = await bridge($, 'reserve', request.id);
    if (!reserved.reserved) return reserved;
    try {
        // A separate dependent plugin submits, so this plugin's prompt hook
        // participates in native admission instead of being skipped recursively.
        const result = await bridge($, 'submit', request.id);
        if (result.drop !== undefined) return { disposition: 'deferred', reason: 'claude_native_prompt_declined' };
        if (result.text === request.payload.instruction && result.origin?.kind === 'plugin' &&
            result.origin.name === 'logonomic-wake-dispatch')
        {
            return { disposition: 'accepted', receipt: request.payload.notification_id };
        }
        return { disposition: 'uncertain', reason: 'claude_wake_result_unknown' };
    }
    catch {
        return { disposition: 'uncertain', reason: 'claude_wake_result_unknown' };
    }
}

async function listen($) {
    while (active) {
        try {
            const request = await bridge($, 'listen');
            if (request) await bridge($, 'reply', request.id, await process_request($, request));
        }
        catch {
            // Process failures disable this registration instead of spinning or
            // claiming the mailbox was read. Ordinary Claude operation continues.
            active = false;
        }
    }
}

export function register(on) {
    on('classic.SessionStart', async ($, event, next) => {
        if (!event.agent_id) {
            try {
                const helper = await $.env.get('VNM_CLAUDE_WAKE_OWNER_HELPER');
                if (helper) {
                    // Native argv execution has no shell or PATH Node shim.
                    // Anchor the CLI before next runs the classic relay, which
                    // allocates storage only after the host accepts its start.
                    await $.process.run([helper, 'wake-owner'], { timeoutMs: 5000 });
                }
            }
            catch { /* An unavailable owner disables wake alone. */ }
        }
        return next(event);
    });
    on('session.start', async ($, event, next) => {
        directory = await $.env.get('VNM_CLAUDE_WAKE_DIRECTORY');
        if (directory && event.isInteractive) {
            native_id = await $.session.id();
            active = true;
            // A reload cannot infer whether a previously interrupted session is
            // paused. An actual composer prompt or completed answer admits it.
            paused = true;
            $.clock.after(0, () => listen($));
        }
        return next(event);
    });
    on('session.end', async ($, event, next) => {
        active = false;
        if (directory) {
            try { await bridge($, 'close'); } catch { /* Wake alone is unavailable. */ }
        }
        return next(event);
    });
    on('turn.start', ($, event, next) => {
        busy = true;
        return next(event);
    });
    on('turn.complete', async ($, event, next) => {
        const completed_id = native_id;
        if (!event.agentId) {
            busy = false;
            paused = event.reason !== 'answer';
        }
        const result = await next(event);
        if (!event.agentId && event.reason === 'answer' && active && native_id === completed_id &&
            await $.session.id() === completed_id)
        {
            try {
                // Classic Stop hooks precede this native completion. Their ACK
                // may already have retried while we were still busy. Publish a
                // fresh authenticated event only after native readiness changes.
                await $.process.run(['node', `${$.plugin.root}/scripts/logonomic-hook-relay.cjs`], {
                    stdin: JSON.stringify({ hook_event_name: 'ModTurnComplete', session_id: completed_id, reason: 'answer' }),
                    timeoutMs: 13000,
                });
            }
            catch { /* Notification failure must not change the completed turn. */ }
        }
        return result;
    });
    on('prompt.submit', async ($, event, next) => {
        if (event.origin.kind === 'composer' || event.origin.kind === 'bridge') paused = false;
        if (event.origin.kind === 'plugin' && event.origin.name === 'logonomic-wake-dispatch') {
            const request = await bridge($, 'guard-request');
            if (event.text !== request.payload.instruction) return { drop: 'Logonomic mailbox notification does not match its request.' };
            const refused = await admission($, request);
            if (refused) return { drop: 'Logonomic mailbox notification deferred: ' + refused.reason };
        }
        return next(event);
    }).catch(($, event, next) => {
        if (!next.called && event.origin.kind === 'plugin' && event.origin.name === 'logonomic-wake-dispatch')
        {
            return { drop: 'Logonomic mailbox notification deferred: native readiness is unavailable' };
        }
        return next(event);
    });
}
