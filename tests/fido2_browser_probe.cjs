const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
(async () => {
    const logs = [], data = new Uint8Array(53);data[32] = 0x45;
    const result = {id: 'PRIVATE_CREDENTIAL_ID', authenticatorAttachment: 'cross-platform',
        response: {clientDataJSON: 'PRIVATE_CLIENT_DATA',
            getAuthenticatorData: () => data.buffer, getTransports: () => ['usb']}};
    let received, fail = false, pending = false, resolvePending;
    const original = function (options) {
        assert.equal(this, credentials);received = options;
        if (pending) return new Promise(resolve => {resolvePending = resolve;});
        return fail ? Promise.reject(new DOMException('PRIVATE_ERROR_DETAIL', 'NotAllowedError')) : Promise.resolve(result);
    };
    const prototype = {create: original, get: original};
    const credentials = Object.create(prototype);
    const context = vm.createContext({navigator: {credentials}, console: {info: (...v) => logs.push(v.join(' '))}});
    vm.runInContext(fs.readFileSync(path.join(__dirname, '../scripts/fido2/browser-diagnostics.js'), 'utf8'), context);
    const options = {publicKey: {challenge: 'PRIVATE_CHALLENGE', user: {id: 'PRIVATE_USER'},
        pubKeyCredParams: [{alg: -7}], attestation: 'direct', authenticatorSelection: {userVerification: 'required'}}};
    assert.equal(await credentials.create(options), result);assert.equal(received, options);
    const record = context.ameFidoDiagnostics.records[0];
    assert.equal(record.outcome, 'success');assert.equal(record.flags.uv, true);
    assert.equal(record.flags.backupEligible, false);assert.equal(record.aaguid, '0'.repeat(32));
    fail = true;await assert.rejects(credentials.get(options), {name: 'NotAllowedError'});
    assert.equal(context.ameFidoDiagnostics.records[1].error, 'NotAllowedError');
    pending = true;
    const waiting = credentials.create(options);
    assert.equal(context.ameFidoDiagnostics.records[2].outcome, 'pending');
    assert(logs.some(line => line.includes('"outcome":"pending"')));
    resolvePending(result);assert.equal(await waiting, result);
    assert.equal(context.ameFidoDiagnostics.records[2].outcome, 'success');
    assert(!JSON.stringify(context.ameFidoDiagnostics.records).includes('PRIVATE_'));
    assert(!logs.join('\n').includes('PRIVATE_'));
    context.ameFidoDiagnostics.restore();assert.equal(credentials.create, original);
    assert(!Object.hasOwn(credentials, 'create'));assert(!context.ameFidoDiagnostics);
    console.log('WebAuthn diagnostics preserve results and redact credential material');
})().catch(error => {console.error(error);process.exitCode = 1;});
