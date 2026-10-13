// Native dependencies keep the admission plugin loaded with this dispatcher.
// Only that plugin creates dispatch requests. The dispatcher has no command,
// tool, public listener, inbox credentials or independently authored prompts.
let active = false;
let directory;
let native_id;

async function bridge($, operation, id, value) {
    const result = await $.process.run(
        ['node', `${$.plugin.root}/../claude-hooks/scripts/logonomic-wake.cjs`, operation, directory, ...(id ? [id] : [])],
        { stdin: value === undefined ? '' : JSON.stringify(value), timeoutMs: 25000 });
    if (result.exitCode !== 0) throw new Error('bridge_unavailable');
    return JSON.parse(result.stdout);
}

async function listen($) {
    while (active) {
        try {
            await bridge($, 'ready', undefined, { session_id: native_id });
            const request = await bridge($, 'receive');
            if (!request) continue;
            let result = { drop: 'Logonomic mailbox notification expired or its session changed.' };
            if (active && await $.session.id() === native_id && request.payload.session.provider_session_id === native_id &&
                (await bridge($, 'activate', request.id)).available)
            {
                // The dependent guard reads the exact active request at native
                // admission, after any native queue wait, and may drop it.
                result = await $.prompt.submit({ text: request.payload.instruction });
            }
            await bridge($, 'native-reply', request.id, result);
        }
        catch { active = false; }
    }
}

export function register(on) {
    on('session.start', async ($, event, next) => {
        directory = await $.env.get('VNM_CLAUDE_WAKE_DIRECTORY');
        if (directory && event.isInteractive) {
            native_id = await $.session.id();
            active = true;
            $.clock.after(0, () => listen($));
        }
        return next(event);
    });
    on('session.end', ($, event, next) => {
        active = false;
        return next(event);
    });
}
