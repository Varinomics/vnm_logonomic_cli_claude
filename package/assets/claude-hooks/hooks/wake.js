// Only public Claude Mods APIs are used here. Claude Code owns submission,
// attribution, permission prompts and cancellation of the running model turn.
let directory;
let interactive = false;
let activation = { active: false, busy: false, paused: true };
// Native session state for model sequencing. A module reload keeps the native
// session and its accepted transition nonce, so a counter held only by this
// module would restart below the host's sequence barrier. /clear, /resume and
// /branch reset the state with the activation.
// - module_load: each module copy writes it once as it loads. The version of
//   that write orders the copies, also after such a reset.
// - model_sequence: { owner, sequence }, the load version of the copy that owns
//   sequencing and the highest sequence published.
const module_load    = { plugin: 'logonomic-claude-hooks', key: 'module_load' };
const model_sequence = { plugin: 'logonomic-claude-hooks', key: 'model_sequence' };
let load_version;

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

// The write is issued synchronously; a failure, including session state the
// engine does not provide, rejects only model sequencing and leaves wake intact.
async function write_load($) {
    return (await $.state.set(module_load, true)).version;
}

// Once its binding is known, a module copy takes over model sequencing for that
// activation and counts on from the highest sequence published. Native events
// reach only the newest copy, but a replaced copy's listener may still finish
// its binding afterwards; its earlier load cannot take ownership back.
async function claim_model_sequence($, current) {
    try {
        const load = await load_version;
        for (;;) {
            const held = await $.state.get(model_sequence);
            if (held.value?.owner > load) return;
            const claimed = { owner: load, sequence: held.value?.sequence ?? '0' };
            if ((await $.state.set(model_sequence, claimed, { ifVersion: held.version })).isSet) {
                current.model_sequence = BigInt(claimed.sequence);
                current.model_owner    = load;
                return;
            }
        }
    }
    catch { /* Without session state this activation reports no model observations. */ }
}

function capture_model(current) {
    if (!current.model_owner || current.binding.provider_session_id !== current.session_id) return;
    return { ...current.binding, owner: current.model_owner, sequence: String(++current.model_sequence) };
}

// Publication requires the capturing copy to still own sequencing and the
// published sequence to cover the observation, so the next owner continues
// above it. A request captured by a module copy that a reload replaced is omitted.
async function commit_model_sequence($, captured) {
    for (;;) {
        const held = await $.state.get(model_sequence);
        if (held.value?.owner !== captured.owner) return false;
        if (BigInt(captured.sequence) <= BigInt(held.value.sequence)) return true;
        const committed = { owner: captured.owner, sequence: captured.sequence };
        if ((await $.state.set(model_sequence, committed, { ifVersion: held.version })).isSet) return true;
    }
}

async function report_model($, current, captured, agent_id, model, source) {
    if (!await matches_activation($, current)) return;
    if (agent_id) {
        // Native compaction/memory forks have loop IDs absent from the agent
        // roster. Only the real child whose step supplied this model qualifies.
        const agents = await $.agent.list();
        if (!agents.some(agent => agent.id === agent_id) || !await matches_activation($, current)) return;
    }
    if (!await commit_model_sequence($, captured) || current !== activation) return;
    await $.process.run(['node', `${$.plugin.root}/scripts/logonomic-hook-relay.cjs`], {
        stdin: JSON.stringify({
            hook_event_name: 'ModModel', session_id: captured.provider_session_id, agent_id,
            model_state: { version: 1, updates: [{
                provider_session_id: captured.provider_session_id,
                transition_nonce: captured.transition_nonce, sequence: captured.sequence,
                agent_id, state: 'confirmed', model, source,
            }] },
        }),
        timeoutMs: 13000,
    });
}

// The host retains one model per actor. Keep its newest pending observation
// while a single relay is busy, reserving a slot for the root at child capacity.
function queue_model($, current, captured, agent_id, model, source) {
    if (!captured || current !== activation || typeof model !== 'string' || !model || model === '<synthetic>') return;
    const pending = current.pending_models ??= new Map();
    const prior = pending.get(agent_id);
    if (prior && BigInt(captured.sequence) < BigInt(prior.captured.sequence)) return;
    if (!prior && agent_id && pending.size - Number(pending.has('')) >= 128) return;
    pending.set(agent_id, { captured, model, source });
    if (current.sending_models) return;
    current.sending_models = true;
    $.clock.after(0, async () => {
        try {
            while (current === activation && pending.size) {
                const [actor, observation] = pending.entries().next().value;
                pending.delete(actor);
                try {
                    await report_model($, current, observation.captured, actor, observation.model, observation.source);
                }
                catch { /* A reporting failure drops only this optional observation. */ }
            }
        }
        finally { current.sending_models = false; }
    });
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
                await claim_model_sequence($, current);
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
    on('turn.step', async function* ($, event, next) {
        // Source order and activation precede the request. A slow old request
        // cannot adopt a later activation or overwrite a newer model switch.
        const current = activation;
        const captured = capture_model(current);
        const result = yield* next(event);
        if (!next.signal?.aborted && result.stopReason !== 'compaction') {
            try {
                queue_model($, current, captured, event.agentId ?? '', result.usage?.model,
                    'claude.turn.step.usage');
            }
            catch { /* Optional model evidence cannot change the native response. */ }
        }
        return result;
    });
    on('classic.PostModelSwitch', async ($, event, next) => {
        const current = activation;
        const captured = capture_model(current);
        const result = await next(event);
        if (!event.agent_id && !next.signal?.aborted && event.session_id === current.session_id) {
            try {
                queue_model($, current, captured, '', event.to_model, 'claude.PostModelSwitch');
            }
            catch { /* The native model switch has already succeeded. */ }
        }
        return result;
    });
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
                // Native /resume switches $.session.id() after this hook returns.
                // The accepted event owns startup; actual admission still checks
                // the live native ID before submitting a prompt.
                if (current === activation) {
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
        // Each fresh copy runs this once as it loads. Issued before anything
        // else is awaited, after every earlier copy's load write.
        load_version = write_load($);
        load_version.catch(() => { /* A listener's claim observes the failure. */ });
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
