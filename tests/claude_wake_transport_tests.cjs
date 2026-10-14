'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const crypto = require('node:crypto');
const { main } = require(path.resolve(process.argv[2]));

const directory = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'claude-wake-contract-')));
const registration = { provider_session_id: 'native-root', transition_nonce: 'b'.repeat(32) };
function request(notification_id = crypto.randomUUID(), duration = 5000) {
    return {
        id: crypto.randomBytes(16).toString('hex'), operation: 'notify', expires_at_ms: Date.now() + duration,
        payload: { session: { provider_session_id: registration.provider_session_id },
            correlation: { transition_nonce: registration.transition_nonce }, notification_id, instruction: 'Read the host mailbox.' },
    };
}
function put(value, suffix = 'request') {
    fs.writeFileSync(path.join(directory, value.id + '.' + suffix), JSON.stringify(value));
}
async function claim(value) {
    const received = main(['listen', directory], '');
    put(value);
    assert.deepEqual(await received, value);
    return value;
}

async function run() {
    const delayed = path.join(directory, 'd'.repeat(32));
    const startup = main(['ready', delayed], JSON.stringify({ session_id: 'native-root' }));
    assert.equal(fs.existsSync(delayed), false);
    fs.mkdirSync(delayed);
    assert.deepEqual(await startup, { completed: true });
    await main(['close', delayed], '');
    assert.equal(fs.existsSync(delayed), false);
    fs.writeFileSync(path.join(directory, 'registration.json'), JSON.stringify(registration));
    assert.deepEqual(await main(['inspect-dispatch', directory], JSON.stringify({ session_id: 'native-root' })), { available: false });
    await main(['ready', directory], JSON.stringify({ session_id: 'native-root' }));
    assert.deepEqual(await main(['inspect-dispatch', directory], JSON.stringify({ session_id: 'native-root' })), { available: true });
    assert.deepEqual(await main(['inspect-dispatch', directory], JSON.stringify({ session_id: 'other-root' })), { available: false });

    const stale = request();
    stale.payload.correlation.transition_nonce = 'c'.repeat(32);
    put(stale);
    const expired = request('expired', -1);
    put(expired);
    const notice = await claim(request('once'));
    assert.equal(fs.existsSync(path.join(directory, stale.id + '.request')), false);
    assert.equal(fs.existsSync(path.join(directory, expired.id + '.request')), false);
    assert.deepEqual(await main(['reserve', directory, notice.id], ''), { reserved: true });
    const submitting = main(['submit', directory, notice.id], '');
    assert.deepEqual(await main(['receive', directory], ''), notice);
    assert.deepEqual(await main(['activate', directory, notice.id], ''), { available: true });
    assert.deepEqual(await main(['guard-request', directory], ''), notice);
    const native = { text: notice.payload.instruction, origin: { kind: 'plugin', name: 'logonomic-wake-dispatch' } };
    await main(['native-reply', directory, notice.id], JSON.stringify(native));
    assert.deepEqual(await submitting, native);
    const accepted = { disposition: 'accepted', receipt: 'once' };
    await main(['reply', directory, notice.id], JSON.stringify(accepted));
    assert.deepEqual(JSON.parse(fs.readFileSync(path.join(directory, notice.id + '.response'))), accepted);
    const duplicate = await claim(request('once'));
    assert.deepEqual(await main(['reserve', directory, duplicate.id], ''), accepted);
    await main(['reply', directory, duplicate.id], JSON.stringify(accepted));

    const deferred = await claim(request('retry'));
    await main(['reserve', directory, deferred.id], '');
    await main(['reply', directory, deferred.id], JSON.stringify({ disposition: 'deferred', reason: 'native_busy' }));
    const retry = await claim(request('retry'));
    assert.deepEqual(await main(['reserve', directory, retry.id], ''), { reserved: true });
    await main(['reply', directory, retry.id], JSON.stringify({ disposition: 'deferred', reason: 'native_busy' }));

    const uncertain = await claim(request('uncertain', 150));
    await main(['reserve', directory, uncertain.id], '');
    assert.deepEqual(await main(['submit', directory, uncertain.id], ''), { unknown: true });
    const retried = await claim(request('uncertain'));
    assert.deepEqual(await main(['reserve', directory, retried.id], ''), {
        disposition: 'uncertain', reason: 'claude_wake_result_unknown',
    });
    const changed = request();
    put(changed, 'submitted');
    fs.writeFileSync(path.join(directory, 'registration.json'), JSON.stringify({ ...registration, provider_session_id: 'new-session' }));
    assert.deepEqual(await main(['activate', directory, changed.id], ''), { available: false });
    await assert.rejects(main(['guard-request', directory], ''));
    fs.writeFileSync(path.join(directory, 'dispatch-ready.json.' + 'e'.repeat(32)), '{}');
    await main(['close', directory], '');
    assert.equal(fs.existsSync(directory), false);
    process.stdout.write('Claude wake transport: correlation, native handoff, replay, expiry and teardown passed.\n');
}

run().catch(error => {
    process.stderr.write(String(error.stack) + '\n');
    process.exitCode = 1;
}).finally(() => {
    // Only the unique directory created by this test is removed.
    fs.rmSync(directory, { recursive: true, force: true });
});
