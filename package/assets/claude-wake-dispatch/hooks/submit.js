// Native dependencies keep the admission plugin loaded with this dispatcher.
// Only that plugin creates dispatch requests. The dispatcher has no command,
// tool, public listener, inbox credentials or independently authored prompts.
let directory;
let interactive = false;
let activation = { active: false };

function replace_activation(session_id, start_pending = false) {
    activation.active = false;
    activation = { session_id, start_pending, active: false };
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

async function bridge($, operation, id, value) {
    const result = await $.process.run(
        ['node', `${$.plugin.root}/../claude-hooks/scripts/logonomic-wake.cjs`, operation, directory, ...(id ? [id] : [])],
        { stdin: value === undefined ? '' : JSON.stringify(value), timeoutMs: 25000 });
    if (result.exitCode !== 0) throw new Error('bridge_unavailable');
    return JSON.parse(result.stdout);
}

async function listen($, current) {
    while (current.active && current === activation) {
        try {
            if (!current.binding) {
                const binding = await bridge($, 'binding', undefined, { session_id: current.session_id });
                if (current !== activation || !current.active) return;
                current.binding = binding;
            }
            await bridge($, 'ready', undefined, current.binding);
            if (current !== activation || !current.active) return;
            const request = await bridge($, 'receive', undefined, current.binding);
            if (current !== activation || !current.active) return;
            if (!request) continue;
            let result = { drop: 'Logonomic mailbox notification expired or its session changed.' };
            if (await matches_activation($, current) && request.payload.session.provider_session_id === current.session_id &&
                request.payload.correlation.transition_nonce === current.binding.transition_nonce &&
                (await bridge($, 'activate', request.id)).available && current === activation && current.active)
            {
                // The dependent guard reads the exact active request at native
                // admission, after any native queue wait, and may drop it.
                result = await $.prompt.submit({ text: request.payload.instruction });
            }
            await bridge($, 'native-reply', request.id, result);
        }
        catch { current.active = false; }
    }
}

export function register(on) {
    on('classic.SessionStart', async ($, event, next) => {
        const current = !event.agent_id && event.source !== 'compact'
            ? replace_activation(event.session_id, true) : activation;
        const result = await next(event);
        if (!event.agent_id && event.source !== 'compact' && !next.signal?.aborted) {
            try {
                // Native /resume switches $.session.id() after this hook returns.
                // Startup follows its accepted event; submission retains the
                // live native-ID and exact binding checks below.
                if (current === activation) {
                    current.start_pending = false;
                    start_listener($, current);
                }
            }
            catch { /* Missing native identity leaves this activation unavailable. */ }
        }
        return result;
    });
    on('session.start', async ($, event, next) => {
        directory = await $.env.get('VNM_CLAUDE_WAKE_DIRECTORY');
        interactive = event.isInteractive;
        const current = activation;
        if (directory && interactive) {
            const session_id = await $.session.id();
            if (current === activation && (!current.session_id || current.session_id === session_id)) {
                current.session_id = session_id;
                start_listener($, current);
            }
        }
        return next(event);
    });
    on('session.end', ($, event, next) => {
        if (!event.sessionId || !activation.session_id || event.sessionId === activation.session_id) {
            replace_activation();
        }
        return next(event);
    });
}
