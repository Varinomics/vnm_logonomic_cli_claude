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
    const received = main(['listen', directory], JSON.stringify(registration));
    put(value);
    assert.deepEqual(await received, value);
    return value;
}

async function run() {
    // Native registration creates its directory before writing and renaming
    // registration.json. Allocation alone must leave binding pending.
    const allocated = path.join(directory, 'a'.repeat(32));
    fs.mkdirSync(allocated);
    let settled = false;
    const publication = main(['binding', allocated], JSON.stringify({ session_id: 'native-root' })).then(
        value => { settled = true; return { value }; },
        error => { settled = true; return { error }; });
    await Promise.resolve();
    assert.equal(settled, false, 'an allocated directory is not a published registration');
    const temporary = path.join(allocated, 'c'.repeat(32) + '.registration');
    fs.writeFileSync(temporary, JSON.stringify(registration));
    fs.renameSync(temporary, path.join(allocated, 'registration.json'));
    assert.deepEqual(await publication, { value: registration });
    await main(['close', allocated], '');

    const delayed = path.join(directory, 'd'.repeat(32));
    const startup = main(['binding', delayed], JSON.stringify({ session_id: 'native-root' }));
    assert.equal(fs.existsSync(delayed), false);
    fs.mkdirSync(delayed);
    fs.writeFileSync(path.join(delayed, 'registration.json'), JSON.stringify(registration));
    assert.deepEqual(await startup, registration);
    await main(['close', delayed], '');
    assert.equal(fs.existsSync(delayed), false);
    fs.writeFileSync(path.join(directory, 'registration.json'), JSON.stringify(registration));
    assert.deepEqual(await main(['binding', directory], JSON.stringify({ session_id: 'native-root' })), registration);
    await assert.rejects(main(['binding', directory], JSON.stringify({ session_id: 'other-root' })), /identity_changed/);
    assert.deepEqual(await main(['inspect-dispatch', directory], JSON.stringify(registration)), { available: false });
    await main(['ready', directory], JSON.stringify(registration));
    assert.deepEqual(await main(['inspect-dispatch', directory], JSON.stringify(registration)), { available: true });
    assert.deepEqual(await main(['inspect-dispatch', directory], JSON.stringify({ ...registration, provider_session_id: 'other-root' })), { available: false });

    // A live consumer from the departing activation must not claim the next
    // activation's request, even when the native ID has returned to A.
    const departed_binding = { ...registration };
    const departed_listener = main(['listen', directory], JSON.stringify(departed_binding));
    registration.transition_nonce = 'a'.repeat(32);
    fs.writeFileSync(path.join(directory, 'registration.json'), JSON.stringify(registration));
    assert.deepEqual(await main(['inspect-dispatch', directory], JSON.stringify(registration)), { available: false });
    await assert.rejects(main(['ready', directory], JSON.stringify(departed_binding)), /identity_changed/);
    const successor = request('successor');
    put(successor);
    await assert.rejects(departed_listener, /identity_changed/);
    assert.equal(fs.existsSync(path.join(directory, successor.id + '.request')), true);
    assert.deepEqual(await main(['listen', directory], JSON.stringify(registration)), successor);
    await main(['reply', directory, successor.id], JSON.stringify({ disposition: 'deferred', reason: 'fixture' }));

    const departed_dispatcher = main(['receive', directory], JSON.stringify(registration));
    registration.transition_nonce = 'd'.repeat(32);
    fs.writeFileSync(path.join(directory, 'registration.json'), JSON.stringify(registration));
    const next_submission = request('successor-submission');
    put(next_submission, 'submit');
    await assert.rejects(departed_dispatcher, /identity_changed/);
    assert.equal(fs.existsSync(path.join(directory, next_submission.id + '.submit')), true);
    assert.deepEqual(await main(['receive', directory], JSON.stringify(registration)), next_submission);
    await main(['native-reply', directory, next_submission.id], JSON.stringify({ drop: 'fixture' }));

    const stale = request();
    stale.payload.correlation.transition_nonce = 'c'.repeat(32);
    put(stale);
    const expired = request('expired', -1);
    put(expired);
    const notice = await claim(request('once'));
    assert.equal(fs.existsSync(path.join(directory, stale.id + '.request')), true, 'a consumer never deletes another activation\'s live request');
    assert.equal(fs.existsSync(path.join(directory, expired.id + '.request')), false);
    assert.deepEqual(await main(['reserve', directory, notice.id], ''), { reserved: true });
    const submitting = main(['submit', directory, notice.id], '');
    assert.deepEqual(await main(['receive', directory], JSON.stringify(registration)), notice);
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

    // Filesystem notification and reservation must finish before expiry; their
    // scheduling is not part of the native-result timeout contract.
    const real_now = Date.now;
    let now = real_now();
    Date.now = () => now;
    try {
        const uncertain = await claim(request('uncertain', 150));
        assert.deepEqual(await main(['reserve', directory, uncertain.id], ''), { reserved: true });
        const submission = main(['submit', directory, uncertain.id], '');
        now += 150;
        assert.deepEqual(await submission, { unknown: true });
    }
    finally {
        Date.now = real_now;
    }
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
