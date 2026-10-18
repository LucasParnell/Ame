// Paste into the registration page's developer console, then retry once.
// Records WebAuthn capabilities and outcomes, excluding account/user details,
// challenges, credential IDs, public keys, signatures and PIN material.
// No requests are sent and registration options/results are passed unchanged.
(() => {
    if (globalThis.ameFidoDiagnostics) throw new Error('Already installed; reload to remove');
    const credentials = navigator.credentials;
    const records = [];
    const originals = new Map();
    const emit = record => {
        console.info('Ame WebAuthn:', JSON.stringify(record));
    };
    for (const operation of ['create', 'get']) {
        const original = credentials[operation];
        const own = Object.getOwnPropertyDescriptor(credentials, operation);
        originals.set(operation, own);
        Object.defineProperty(credentials, operation, {configurable: true, writable: true, value: function (...args) {
            const options = args[0]?.publicKey;
            if (!options) return Reflect.apply(original, this, args);
            const record = {
                operation,
                outcome: 'pending',
                attestation: options.attestation ?? 'none',
                selection: options.authenticatorSelection ? {
                    attachment: options.authenticatorSelection.authenticatorAttachment,
                    residentKey: options.authenticatorSelection.residentKey,
                    requireResidentKey: options.authenticatorSelection.requireResidentKey,
                    userVerification: options.authenticatorSelection.userVerification,
                } : undefined,
                algorithms: options.pubKeyCredParams?.map(item => item.alg),
                excludedCount: options.excludeCredentials?.length,
                allowedCount: options.allowCredentials?.length,
            };
            // Log before opening the native prompt so a stalled request can be
            // distinguished from a hook installed in the wrong page/frame.
            records.push(record);
            try { emit(record); } catch {}
            const rejected = error => {
                try {record.outcome = 'error';record.error = error.name;emit(record);} catch {}
                throw error;
            };
            let promise;
            try { promise = Reflect.apply(original, this, args); }
            catch (error) { return rejected(error); }
            return promise.then(result => {
                try {
                    record.outcome = result ? 'success' : 'no credential';
                    record.attachment = result?.authenticatorAttachment;
                    const response = result?.response;
                    record.transports = response?.getTransports?.();
                    const data = response?.getAuthenticatorData?.() ?? response?.authenticatorData;
                    if (data) {
                        const bytes = new Uint8Array(data), flags = bytes[32];
                        if (bytes.length >= 37) record.flags = {
                            up: !!(flags & 1), uv: !!(flags & 4),
                            backupEligible: !!(flags & 8), backedUp: !!(flags & 16),
                        };
                        if (bytes.length >= 53 && (flags & 64))
                            record.aaguid = Array.from(bytes.slice(37, 53), b => b.toString(16).padStart(2, '0')).join('');
                    }
                    emit(record);
                } catch { /* Diagnostics must not change registration. */ }
                return result;
            }, rejected);
        }});
    }
    globalThis.ameFidoDiagnostics = {
        records,
        restore() {
            for (const [operation, descriptor] of originals) {
                if (descriptor) Object.defineProperty(credentials, operation, descriptor);
                else delete credentials[operation];
            }
            delete globalThis.ameFidoDiagnostics;
        },
    };
    console.info('Ame WebAuthn diagnostics installed. Retry once; reload to remove.');
})();
