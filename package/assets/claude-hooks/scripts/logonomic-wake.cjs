'use strict';

// Provider-private transport. Native submission stays in the loaded Mod; this
// helper never opens a terminal, resumes a session, or speaks native inbox wire.
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');

function read_json(file) {
    const stat = fs.lstatSync(file);
    if (!stat.isFile() || stat.isSymbolicLink() || stat.size > 65536) throw new Error('invalid_file');
    return JSON.parse(fs.readFileSync(file, 'utf8'));
}

function remove(file) {
    try { fs.unlinkSync(file); } catch (error) { if (error.code !== 'ENOENT') throw error; }
}

function atomic_json(file, value) {
    const temporary = file + '.' + crypto.randomBytes(16).toString('hex');
    fs.writeFileSync(temporary, JSON.stringify(value), { flag: 'wx', mode: 0o600 });
    fs.renameSync(temporary, file);
}

function identity_matches(directory, request) {
    const registered = read_json(path.join(directory, 'registration.json'));
    return registered.provider_session_id === request.payload.session.provider_session_id &&
        registered.transition_nonce === request.payload.correlation.transition_nonce;
}

function claim(directory, suffix) {
    for (const name of fs.readdirSync(directory)) {
        if (!new RegExp('^[0-9a-f]{32}\\.' + suffix + '$').test(name)) continue;
        const pending = path.join(directory, name);
        let request;
        try {
            request = read_json(pending);
            if (request.id + '.' + suffix !== name || !Number.isSafeInteger(request.expires_at_ms) ||
                request.expires_at_ms <= Date.now() || !identity_matches(directory, request))
            {
                remove(pending);
                continue;
            }
            fs.renameSync(pending, path.join(directory, request.id + (suffix === 'submit' ? '.submitted' : '.claimed')));
        }
        catch { continue; }
        return request;
    }
    return null;
}

async function listen(directory, suffix) {
    const first = claim(directory, suffix);
    if (first) return first;
    return new Promise((resolve, reject) => {
        let done = false;
        const finish = (value, error) => {
            if (done) return;
            done = true;
            clearTimeout(timer);
            watcher.close();
            if (error) reject(error); else resolve(value);
        };
        const scan = () => {
            try {
                const request = claim(directory, suffix);
                if (request) finish(request);
            }
            catch (error) { finish(null, error); }
        };
        const watcher = fs.watch(directory, scan);
        const timer = setTimeout(() => finish(null), 20000);
        watcher.on('error', error => finish(null, error));
        // Close the gap between the first scan and installing the watcher.
        scan();
    });
}

async function native_result(directory, request) {
    const file = path.join(directory, request.id + '.native-response');
    const read = () => {
        try { return read_json(file); }
        catch (error) { if (error.code !== 'ENOENT') throw error; }
        return null;
    };
    const first = read();
    if (first) return first;
    return new Promise((resolve, reject) => {
        let done = false;
        const finish = (value, error) => {
            if (done) return;
            done = true;
            clearTimeout(timer);
            watcher.close();
            if (error) reject(error); else resolve(value);
        };
        const scan = () => {
            try {
                const value = read();
                if (value) finish(value);
            }
            catch (error) { finish(null, error); }
        };
        const watcher = fs.watch(directory, scan);
        const timer = setTimeout(() => finish({ unknown: true }), Math.max(0, request.expires_at_ms - Date.now()));
        watcher.on('error', error => finish(null, error));
        scan();
    });
}

function receipt_path(directory, notification_id) {
    if (typeof notification_id !== 'string' || !notification_id || notification_id.length > 256) {
        throw new Error('invalid_notification');
    }
    return path.join(directory, crypto.createHash('sha256').update(notification_id).digest('hex') + '.receipt');
}

async function main(argv, input) {
    const [operation, directory, id] = argv;
    if (!path.isAbsolute(directory) || fs.lstatSync(directory).isSymbolicLink() ||
        path.normalize(fs.realpathSync(directory)) !== path.normalize(directory)) throw new Error('invalid_directory');
    if (operation === 'listen' || operation === 'receive') {
        return listen(directory, operation === 'listen' ? 'request' : 'submit');
    }
    if (operation === 'ready') {
        atomic_json(path.join(directory, 'dispatch-ready.json'), {
            session_id: JSON.parse(input).session_id, expires_at_ms: Date.now() + 30000,
        });
        return { completed: true };
    }
    if (operation === 'inspect-dispatch') {
        try {
            const ready = read_json(path.join(directory, 'dispatch-ready.json'));
            return { available: ready.session_id === JSON.parse(input).session_id && ready.expires_at_ms > Date.now() };
        }
        catch { return { available: false }; }
    }
    if (operation === 'guard-request') {
        const active = read_json(path.join(directory, 'active.json'));
        if (!/^[0-9a-f]{32}$/.test(active.id)) throw new Error('invalid_id');
        const request = read_json(path.join(directory, active.id + '.submitted'));
        if (!identity_matches(directory, request)) throw new Error('identity_changed');
        return request;
    }
    if (operation === 'close') {
        for (const name of fs.readdirSync(directory)) {
            if (['registration.json', 'dispatch-ready.json', 'active.json'].includes(name) ||
                /^[0-9a-f]{32}\.(request|claimed|response|writing|registration|submit|submitted|native-response)$/.test(name) ||
                /^[0-9a-f]{64}\.receipt(\.[0-9a-f]{32})?$/.test(name)) remove(path.join(directory, name));
        }
        try { fs.rmdirSync(directory); } catch { /* A watcher may still be closing. */ }
        return { completed: true };
    }
    if (!/^[0-9a-f]{32}$/.test(id)) throw new Error('invalid_id');
    if (operation === 'activate') {
        const request = read_json(path.join(directory, id + '.submitted'));
        if (request.expires_at_ms <= Date.now() || !identity_matches(directory, request)) return { available: false };
        atomic_json(path.join(directory, 'active.json'), { id });
        return { available: true };
    }
    if (operation === 'native-reply') {
        const request = read_json(path.join(directory, id + '.submitted'));
        if (request.expires_at_ms > Date.now()) atomic_json(path.join(directory, id + '.native-response'), JSON.parse(input));
        remove(path.join(directory, id + '.submitted'));
        return { completed: true };
    }
    const request = read_json(path.join(directory, id + '.claimed'));
    if (operation === 'submit') {
        if (request.expires_at_ms <= Date.now() || !identity_matches(directory, request)) return { drop: 'expired' };
        atomic_json(path.join(directory, id + '.submit'), request);
        const result = await native_result(directory, request);
        remove(path.join(directory, id + '.native-response'));
        return result;
    }
    if (operation === 'reserve') {
        if (request.expires_at_ms <= Date.now() || !identity_matches(directory, request)) {
            return { disposition: 'deferred', reason: 'claude_wake_expired' };
        }
        const file = receipt_path(directory, request.payload.notification_id);
        const uncertain = { disposition: 'uncertain', reason: 'claude_wake_result_unknown' };
        try {
            fs.writeFileSync(file, JSON.stringify({ id, result: uncertain }), { flag: 'wx', mode: 0o600 });
            return { reserved: true };
        }
        catch (error) {
            if (error.code !== 'EEXIST') throw error;
            return read_json(file).result;
        }
    }
    if (operation !== 'reply') throw new Error('invalid_operation');
    const result = JSON.parse(input);
    if (request.operation === 'notify') {
        const file = receipt_path(directory, request.payload.notification_id);
        try {
            const saved = read_json(file);
            if (saved.id === id) {
                if (result.disposition === 'accepted' || result.disposition === 'uncertain') {
                    atomic_json(file, { id, result });
                }
                else remove(file);
            }
        }
        catch (error) { if (error.code !== 'ENOENT') throw error; }
    }
    // Late acceptance is retained in the receipt, but no abandoned response
    // file accumulates after the adapter has stopped waiting.
    if (request.expires_at_ms > Date.now()) atomic_json(path.join(directory, id + '.response'), result);
    remove(path.join(directory, id + '.claimed'));
    return { completed: true };
}

module.exports = { main };
if (require.main === module) {
    let input = '';
    process.stdin.setEncoding('utf8');
    process.stdin.on('data', chunk => {
        input += chunk;
        if (input.length > 65536) process.exit(1);
    });
    process.stdin.on('end', () => {
        main(process.argv.slice(2), input).then(result => {
            process.stdout.write(JSON.stringify(result));
        }).catch(() => { process.exitCode = 1; });
    });
}
