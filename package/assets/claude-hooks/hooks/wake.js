// Only public Claude Mods APIs are used here. Claude Code owns submission,
// attribution, permission prompts and cancellation of the running model turn.
let directory;
let interactive = false;
let activation = { active: false, busy: false, paused: true };

function replace_activation(session_id, start_pending = false) {
    activation.active = false;
    activation = { session_id, start_pending, active: false, busy: false, paused: true };
    return activation;
}

async function matches_activation($, current) {
    return current === activation && current.session_id &&
        await $.session.id() === current.session_id && current === activation;
}

function start_listener($, current) {
    if (current !== activation || current.start_pending || current.active || !directory || !interactive) return;
    current.active = true;
    $.clock.after(0, () => listen($, current));
}

async function report_context($, current, payload) {
    if (current !== activation || !current.session_id) return;
    let context_state = { usage_unavailable: true };
    try {
        const usage   = await $.session.usage();
        const context = usage.context;
        if (Number.isSafeInteger(context?.tokens) && context.tokens >= 0 &&
            Number.isSafeInteger(context?.window) && context.window > 0)
        {
            context_state = { used_tokens: context.tokens, context_window: context.window };
        }
    }
    catch { /* A missing native measurement clears the previous reading. */ }
    if (!await matches_activation($, current)) {
        return;
    }
    await $.process.run(['node', `${$.plugin.root}/scripts/logonomic-hook-relay.cjs`], {
        stdin: JSON.stringify({ ...payload, session_id: current.session_id, context_state }),
        timeoutMs: 13000,
    });
}

async function bridge($, operation, id, value) {
    const result = await $.process.run(
        ['node', `${$.plugin.root}/scripts/logonomic-wake.cjs`, operation, directory, ...(id ? [id] : [])],
        { stdin: value === undefined ? '' : JSON.stringify(value), timeoutMs: 25000 });
    if (result.exitCode !== 0) throw new Error('bridge_unavailable');
    return JSON.parse(result.stdout);
}

async function admission($, request, current) {
    if (!current.active || !current.binding || !await matches_activation($, current) ||
        request.payload.session.provider_session_id !== current.session_id ||
        request.payload.correlation.transition_nonce !== current.binding.transition_nonce)
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
    if (current.busy) return { disposition: 'deferred', reason: 'claude_turn_busy' };
    if (current.paused) return { disposition: 'deferred', reason: 'claude_session_paused' };
    if ((await $.prompt.read()).text !== '') return { disposition: 'deferred', reason: 'claude_prompt_draft' };
    if (!current.active || !await matches_activation($, current)) {
        return { disposition: 'unavailable', reason: 'claude_native_session_changed' };
    }
    return null;
}

async function process_request($, request, current) {
    const refused = await admission($, request, current);
    const dispatch = refused ? null : await bridge($, 'inspect-dispatch', undefined, current.binding);
    const unavailable = refused ?? (dispatch.available ? null : { disposition: 'unavailable', reason: 'claude_dispatch_not_ready' });
    if (request.operation === 'inspect') {
        return unavailable ? { available: false, reason: unavailable.reason } : { available: true };
    }
    if (request.operation !== 'notify') return { disposition: 'unavailable', reason: 'claude_wake_operation_unknown' };
    if (unavailable) return unavailable;
    const reserved = await bridge($, 'reserve', request.id);
    if (!reserved.reserved) return reserved;
    if (current !== activation || !current.active) {
        return { disposition: 'unavailable', reason: 'claude_native_session_changed' };
    }
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

async function listen($, current) {
    while (current.active && current === activation) {
        try {
            if (!current.binding) {
                const binding = await bridge($, 'binding', undefined, { session_id: current.session_id });
                if (current !== activation || !current.active) return;
                current.binding = binding;
            }
            const request = await bridge($, 'listen', undefined, current.binding);
            if (current !== activation || !current.active) return;
            if (request) await bridge($, 'reply', request.id, await process_request($, request, current));
        }
        catch {
            // Process failures disable this registration instead of spinning or
            // claiming the mailbox was read. Ordinary Claude operation continues.
            current.active = false;
        }
    }
}

export function register(on) {
    on('classic.SessionStart', async ($, event, next) => {
        // Capture the activation before the first asynchronous operation. A
        // resumed A after B has a different instance from the departing A.
        const current = !event.agent_id && event.source !== 'compact'
            ? replace_activation(event.session_id, true) : activation;
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
        const result = await next(event);
        if (!event.agent_id && event.source !== 'compact' && !next.signal?.aborted) {
            try {
                if (await matches_activation($, current)) {
                    // Bootstrap may initialize the module while this classic
                    // start awaits native registration. This completion owns
                    // permission to start the activation's first consumer.
                    current.start_pending = false;
                    start_listener($, current);
                    await report_context($, current, { hook_event_name: 'ModContext' });
                }
            }
            catch { /* Context telemetry must not change native startup. */ }
        }
        return result;
    });
    on('session.start', async ($, event, next) => {
        directory = await $.env.get('VNM_CLAUDE_WAKE_DIRECTORY');
        interactive = event.isInteractive;
        // A native start may finish while the environment lookup is pending.
        // Bind bootstrap to that activation after its listener prerequisites.
        const current = activation;
        if (directory && interactive) {
            const session_id = await $.session.id();
            // A reload cannot infer whether a previously interrupted session is
            // paused. An actual composer prompt or completed answer admits it.
            if (current === activation && (!current.session_id || current.session_id === session_id)) {
                current.session_id = session_id;
                current.paused = true;
                start_listener($, current);
            }
        }
        return next(event);
    });
    on('session.end', ($, event, next) => {
        if (!event.sessionId || !activation.session_id || event.sessionId === activation.session_id) {
            replace_activation();
        }
        // Classic SessionEnd owns authenticated registration cleanup. A Mod
        // callback must not delete a successor's registration after a switch.
        return next(event);
    });
    on('turn.start', ($, event, next) => {
        if (!event.agentId) activation.busy = true;
        return next(event);
    });
    on('turn.complete', async ($, event, next) => {
        const current = activation;
        if (!event.agentId) {
            try { current.session_id ??= await $.session.id(); }
            catch { /* A missing native identity cannot own a measurement. */ }
            current.busy = false;
            current.paused = event.reason !== 'answer';
        }
        const result = await next(event);
        if (!event.agentId && current.session_id && !next.signal?.aborted) {
            try {
                // Classic Stop hooks precede this native completion. Their ACK
                // may already have retried while we were still busy. Publish a
                // fresh authenticated event only after native readiness changes.
                await report_context($, current, {
                    hook_event_name: event.reason === 'answer' && current.active && current === activation
                        ? 'ModTurnComplete' : 'ModContext',
                    reason: event.reason,
                });
            }
            catch { /* Notification failure must not change the completed turn. */ }
        }
        return result;
    });
    on('prompt.submit', async ($, event, next) => {
        const current = activation;
        if (event.origin.kind === 'composer' || event.origin.kind === 'bridge') current.paused = false;
        if (event.origin.kind === 'plugin' && event.origin.name === 'logonomic-wake-dispatch') {
            const request = await bridge($, 'guard-request');
            if (event.text !== request.payload.instruction) return { drop: 'Logonomic mailbox notification does not match its request.' };
            const refused = await admission($, request, current);
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
