# Ame FIDO2 attestation (direct) — implementation plan

Status: proposed, 17 October 2026.
Target: `usb.fn.fido2` in the working tree at `0d4108b` (the `fido2/`
directory is currently untracked).
Goal: make `authenticatorMakeCredential` return a cryptographically valid
`packed` attestation statement carrying Ame's own AAGUID — first self-attested,
then certificate-backed — and verify it independently of any browser.
Non-goal: making OpenAI Daybreak trust the result. That is a separate trust
policy question, covered by the decision gate in §9.

## Implementation status (17 October 2026)

Phases 0–2 are implemented in the working tree, with the certificate path
compile-tested through `usb.fn.fido2` and verified by the host probe:

- `userland/usb/functions/fido2/attestation.h` carries Ame's model AAGUID.
- `authenticator.h` returns the AAGUID from GetInfo and authData and emits
  `packed` self-attestation or certificate-backed `packed` attestation.
- `scripts/fido2/attestation.py` creates the development CA, issues the leaf,
  emits `attestation_cert.h`, and verifies the certificate profile.
- `scripts/fido2/verify_attestation.py` performs an independent CTAP2 check
  (including optional ClientPIN) and verifies the leaf signature and chain.
- `tests/test_fido2.py` covers self-attestation, certificate attestation,
  OpenSSL signature/chain verification, the AAGUID extension, and tamper
  rejection.
- `userland/CMakeLists.txt` adds `AME_FIDO2_ATTESTATION` and the generated
  certificate header; the option is off by default.
- `-fstack-usage` at `-O0` reports `Authenticator::finish` at 2368 bytes
  (2352 before attestation) and `prepare_attestation` at 224 bytes, against a
  4096-byte service stack. The authData-tail reuse kept the hot path flat.

Still to run on hardware: `verify_attestation.py --pin ... --ca ...`, Chrome
and Safari capture, and the Daybreak trial. Phase 4 (seed capability v2) is
optional deployment hardening and is not required to test the statement.

## 1. What exists today

The CTAP2 implementation is header-only under
`userland/usb/functions/fido2/`:

| File | Role |
|---|---|
| `authenticator.h` | CTAP2 command parsing, `authenticatorGetInfo`, MakeCredential/GetAssertion, authData and response encoding |
| `p256.h` | uECC P-256 key derivation and deterministic ECDSA, DER output |
| `crypto.h` | BLAKE2b PRF, HMAC-SHA256, AES-CBC for ClientPIN |
| `store.h` / `fs_store.h` | Sealed resident-credential and PIN state |
| `hid.h` | CTAPHID framing, presence gating |
| `runtime.h` / `main.cc` | Service loop, seed capability, prompt channel |

Current registration response:

```text
fmt:     "none"
attStmt: {}
AAGUID:  00000000-0000-0000-0000-000000000000
```

Two places hardcode the zero AAGUID:

- `Authenticator::info()` (`authenticator.h:76`) writes a local
  `mx_u8_t aaguid[16]{}` as GetInfo key `0x03`.
- `Authenticator::finish()` (`authenticator.h:237`) writes 16 zero bytes at
  authData offset 37 (`authenticator.h:285`).

Registration always emits `w.text("none")` and `w.map(0)` at
`authenticator.h:297-299`.

Relevant constraints measured from the code:

- `kMessageSize` and `Authenticator::response` are 2048 bytes.
- `Authenticator::finish()` builds authData in a 256-byte local buffer.
- ES256 authData is 197 bytes: 37-byte header + 16-byte AAGUID + 2-byte
  credential length + 65-byte credential ID + 77-byte COSE key. A measured
  ES256 registration response is 210 bytes total with `fmt: none` and an
  empty attStmt.
- EdDSA authData is 162 bytes; the existing probe asserts this.
- Userland service stack is 4096 bytes (`scripts/mx/mxld.py` default). Extra
  crypto locals must be budgeted, not just added.
- Host regression baseline passes: `python3 tests/test_fido2.py -v` → 3 tests,
  ~14 s.

CTAP2 has no "direct" flag. `attestation: "direct"` is a WebAuthn client
option; the authenticator always returns the statement it can produce, and the
client decides whether to forward or replace it. This plan changes what the
authenticator produces; it cannot force Safari to pass it through.

## 2. Target behaviour

After this work, a registration with an ES256 credential returns:

```text
fmt:     "packed"
attStmt: { alg: -7, sig: <DER ECDSA>, x5c: [<leaf DER>] }
AAGUID:  <Ame's 16-byte model AAGUID>
```

and the signature verifies over:

```text
authData || clientDataHash
```

using the leaf certificate's P-256 public key. `x5c` contains the leaf only;
the development CA is distributed out of band. For an EdDSA credential the
credential public key in authData stays EdDSA, while the attestation statement
is still ES256 and signed by the dedicated attestation key.

Fallback ladder:

1. Certificate-backed `packed` when a valid attestation cert and matching key
   are available.
2. Self-attested `packed` for ES256 credentials when no cert is available.
3. `none` for EdDSA credentials when no cert is available.
4. `0x7F` (other) if signing was attempted and failed; never emit a malformed
   statement.

## 3. Design decisions

### 3.1 Packed, not fido-u2f/tpm/android-key

`packed` is the generic WebAuthn attestation format for standalone
authenticators. It supports the existing ES256 credential path, and a
dedicated ES256 attestation key can attest both ES256 and EdDSA credentials.
`fido-u2f` would force a U2F-shaped certificate and signature format;
`tpm` and `android-key` require platform facilities Ame does not have.
`none` is the current state and carries no provenance.

### 3.2 One AAGUID, generated once

The AAGUID identifies the authenticator model, not a credential or a device.
Generate a UUIDv4 once, commit its 16 bytes, and never regenerate it per build:

```sh
uuidgen
```

Put the bytes and a readable copy in a new
`userland/usb/functions/fido2/attestation.h`:

```cpp
inline constexpr mx_u8_t kAaguid[16] = { /* 16 bytes */ };
inline constexpr char kAaguidText[] = "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx";
```

Both `info()` and `finish()` must read this one constant. A test asserts the
GetInfo bytes, the authData bytes and the certificate extension all agree.

### 3.3 Attestation key derived from the master seed

Use the existing keyed-BLAKE2b derivation instead of adding a new secret to
the sealed store:

```cpp
derive(attestation_secret, 5, {});   // domain 5 is unused today
```

Domains currently in use: 1/2 (credential ID), 3/4 (credential key),
8/9 (store sealing), 10/11 (ClientPIN). Domain 5 is free.

Advantages:

- No `Stored` layout change, no `SealedStore` version bump, no migration;
  existing resident credentials and PIN state remain valid.
- The attestation key is domain-separated from every credential key; leaking
  one does not reveal the other.
- Deterministic: the host provisioning script can derive the same public key
  from the seed it already generates.

Limitation: the key is software-protected and as extractable as the master
seed. This is explicitly not a hardware-backed non-exportable key.

If the derived scalar is invalid (probability ≈ 2⁻¹²⁸), retry with domain 5
plus a one-byte attempt counter, mirroring the existing ES256 credential
retry at `authenticator.h:270-275`. The Python provisioner must implement the
same loop.

### 3.4 Certificate-backed statement

Generate a separate P-256 attestation key pair from the master seed and issue
an X.509 leaf from a development CA. The leaf profile is in §6.

`x5c` contains the leaf DER only. The root is distributed to the test host as
`dev-ca.pem`; it is not embedded in the response and is not installed into the
macOS trust store by this plan.

The certificate's public key must match the runtime-derived attestation key.
The generated header carries `kAttestationPublicKey[64]`; `main.cc` calls
`prepare_attestation()` at startup, which derives the key and compares it. On
mismatch, log and fall back to self-attestation rather than emitting a
statement that cannot verify.

### 3.5 Certificate delivery

Two options, deliberately staged:

**Phase 2 (bring-up): generated userland header.**
`scripts/fido2/attestation.py` reads the same 32-byte seed used by
`scripts/fido2/seed.py`, derives the attestation key, issues the leaf, and
emits `attestation_cert.h` into the build directory. The cert is public data,
so embedding it in `.rodata` is safe. This is the fastest path to a testable
artifact, but it couples the userland bundle to one device's seed.

**Phase 4 (landing properly): `MxFido2Seed` version 2.**
Move the leaf, its public key and the AAGUID into the private boot capability
page that already carries the master seed. `mochi_system` stays
device-agnostic; only the kernel image is per-device. Details in §4 Phase 4.

Phase 2 is enough to answer the interoperability question. Phase 4 is required
before treating the feature as part of the normal build.

### 3.6 Privacy: model cert vs per-device cert

A per-device leaf is unique and therefore trackable across relying parties;
a model-level leaf shared by all Ame devices is the FIDO-conventional choice.
Because this implementation derives the key from a per-device seed, Phase 2
produces per-device leaves. That is acceptable for a development authenticator
and for a single test handset, but it is a privacy regression relative to a
model cert. Before any wider distribution, decide between:

- a model-level attestation key baked into firmware (shared leaf, key
  extractable from the image), or
- batch keys issued per production run, or
- per-device keys kept in TrustedCore (out of scope here).

Record the decision in this document before changing the provisioning model.

### 3.7 Failure behaviour

- Attestation signing failure → CTAP `0x7F`, no partial response.
- Cert/key mismatch at startup → disable certificate attestation; use self
  attestation (ES256) or `none` (EdDSA).
- Response buffer exhaustion → `Writer::good == false`; return `0x7F`.
- Never silently downgrade from certificate-backed to self-attestation or
  `none` without a log marker; downgrade is observable in `ctap.status` / a
  new counter.

## 4. Phases

Effort is indicative, not a commitment.

### Phase 0 — Freeze identity and baseline (0.5 day)

- Generate the AAGUID once with `uuidgen`; record the UUID and bytes in
  `attestation.h`.
- Add a test that the constant is 16 bytes, non-zero, and that `kAaguidText`
  matches the bytes.
- Re-run the existing host regression to confirm the baseline.

Acceptance: no behavioural change yet; baseline still passes.

### Phase 1 — AAGUID and packed self-attestation (1 day)

Changes in `authenticator.h`:

1. `info()`: replace the local zero array with
   `w.bytes(libk::as_bytes(kAaguid));`.
2. `finish()`: replace the 16 zero bytes in authData with
   `a.raw(libk::as_bytes(kAaguid));`.
3. For `command == 1` and an ES256 credential, build the packed statement.
   The fragment below sits inside the existing `Writer w{...}` block, after
   authData has been built:

```cpp
// auth already contains authData; append clientDataHash in the spare tail.
const auto message_size = a.size + 32;
if (!a.good || message_size > sizeof(auth) || client_hash.size != 32) {
    status(0x7f); return;
}
libk::copy_n(libk::as_bytes(auth + a.size, 32), client_hash, 32);

mx_u8_t signature[72];
const auto signature_size = p256::sign(
    secret, libk::as_bytes(auth, message_size), signature);
if (!signature_size) { status(0x7f); return; }

w.uint(1); w.text("packed");
w.uint(2); w.bytes(libk::as_bytes(auth, a.size));
w.uint(3); w.map(2);
w.text("alg"); w.item(1, 6);          // ES256 = -7
w.text("sig"); w.bytes(libk::as_bytes(signature, signature_size));
```

4. Keep `none` for EdDSA when no certificate attestation is configured. Do not
   attempt `packed` with an EdDSA self-signature in this phase; some verifiers
   do not implement that combination, and the certificate path in Phase 2
   covers EdDSA credentials anyway.

Why append the client hash to `auth` instead of adding a new
`mx_u8_t message[256]`: the service stack is 4 KB and `p256::sign` already
allocates a ~340-byte `Hash` context plus uECC working state. `auth` is 256
bytes, authData is at most 197 bytes, so the 32-byte tail is free and the
extra stack cost is one 72-byte signature buffer.

Acceptance: ES256 registration returns `fmt: packed`, `alg: -7`, a DER
signature of 70–72 bytes; the signature verifies with the credential public
key from authData. EdDSA registration still returns `none`. GetAssertion,
PIN, resident credentials and presence gating are unchanged.

### Phase 2 — Certificate-backed packed attestation (2–3 days)

New/changed files:

- `userland/usb/functions/fido2/attestation.h`
- `userland/usb/functions/fido2/authenticator.h`
- `userland/usb/functions/fido2/main.cc`
- `scripts/fido2/attestation.py`
- `userland/CMakeLists.txt`
- `tests/fido2_probe.cc`
- `tests/test_fido2.py`

Implementation:

1. Add `Authenticator::prepare_attestation()`:

```cpp
bool attestation_ready = false;

bool prepare_attestation() {
    attestation_ready = false;
#if defined(AME_FIDO2_ATTESTATION) && AME_FIDO2_ATTESTATION
    if (!provisioned) return false;
    mx_u8_t secret[32]{}, public_key[64]{};
    derive(secret, 5, {});
    if (p256::public_key(secret, public_key) &&
        integrity::bytes_equal(public_key, kAttestationPublicKey, 64)) {
        attestation_ready = true;
    }
    crypto_wipe(secret, sizeof(secret));
#endif
    return attestation_ready;
}
```

Call it once in `main.cc` after the master seed is copied and
`provisioned` is set, and once from the test fixture's `provision()` helper.
Do not call it per request.

2. In `finish()`, choose certificate-backed, self-attested or `none`:

```cpp
const bool certificate = command == 1 && attestation_ready;
const bool self = command == 1 && !certificate && es256;
```

For `certificate`, derive the attestation key into the already-wiped `seed`
buffer, sign `auth || client_hash`, wipe it, and write `alg`, `sig`, `x5c`.
For `self`, sign with `secret` as in Phase 1. For neither, write `none` as
today.

3. `main.cc` logs one line at startup:
   `attestation: packed/x5c` when the certificate key matches, otherwise
   `attestation: packed/self (certificate unavailable)`. EdDSA credentials
   still fall back to `none` when no certificate is available. Add a
   `mochi::log_counter("ctap.attestation", ...)` with 0/1/2 for
   none/self/full if the existing log counters are a good fit. Never log the
   seed, attestation private key, certificate private key or client data hash.

4. `scripts/fido2/attestation.py` commands:

```text
init-ca  --key secrets/ame-fido2-dev-ca.key --cert secrets/ame-fido2-dev-ca.pem
issue    --seed <32-byte file> --aaguid-header userland/usb/functions/fido2/attestation.h
         --ca-key ... --ca-cert ...
         --out-header <build>/generated/attestation_cert.h
         --out-cert <build>/generated/attestation_leaf.der
verify   --cert <leaf.der> --ca <ca.pem> --aaguid-header ...
```

`issue` derives the key exactly as `Authenticator::derive` does:

```python
def derive(master, domain, parts=()):
    h = hashlib.blake2b(digest_size=32, key=master)
    h.update(bytes([domain]))
    slots = list(parts) + [b""] * (3 - len(parts))
    for part in slots:
        h.update(len(part).to_bytes(8, "little"))
        h.update(part)
    return h.digest()
```

and emits:

```cpp
inline constexpr mx_u8_t kAttestationCertificate[] = { /* DER */ };
inline constexpr mx_u32_t kAttestationCertificateSize =
    sizeof(kAttestationCertificate);
inline constexpr mx_u8_t kAttestationPublicKey[64] = { /* X || Y */ };
static_assert(kAttestationCertificateSize <= 1024,
              "attestation certificate exceeds response budget");
```

5. CMake:

```cmake
set(AME_FIDO2_ATTESTATION OFF CACHE BOOL "Enable packed x5c attestation")
set(AME_FIDO2_SEED_FILE "" CACHE FILEPATH "Private 32-byte FIDO2 seed")
set(AME_FIDO2_CA_KEY "" CACHE FILEPATH "Development attestation CA key")
set(AME_FIDO2_CA_CERT "" CACHE FILEPATH "Development attestation CA cert")

if(AME_FIDO2_ATTESTATION)
    if(NOT AME_FIDO2_SEED_FILE OR NOT AME_FIDO2_CA_KEY OR NOT AME_FIDO2_CA_CERT)
        message(FATAL_ERROR "AME_FIDO2_ATTESTATION requires seed and CA paths")
    endif()
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/generated/attestation_cert.h
               ${CMAKE_BINARY_DIR}/generated/attestation_leaf.der
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/fido2/attestation.py issue
                --seed ${AME_FIDO2_SEED_FILE}
                --aaguid-header ${CMAKE_SOURCE_DIR}/userland/usb/functions/fido2/attestation.h
                --ca-key ${AME_FIDO2_CA_KEY}
                --ca-cert ${AME_FIDO2_CA_CERT}
                --out-header ${CMAKE_BINARY_DIR}/generated/attestation_cert.h
                --out-cert ${CMAKE_BINARY_DIR}/generated/attestation_leaf.der
        DEPENDS ${AME_FIDO2_SEED_FILE} ${AME_FIDO2_CA_KEY} ${AME_FIDO2_CA_CERT}
                ${CMAKE_SOURCE_DIR}/scripts/fido2/attestation.py
        VERBATIM)
    target_include_directories(usb.fn.fido2 PRIVATE ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(usb.fn.fido2 PRIVATE AME_FIDO2_ATTESTATION=1)
endif()
```

Default OFF keeps ordinary/CI builds working without a private seed. A device
build that wants certificate attestation must set the option; a missing seed
fails at configure time rather than silently shipping `none`. Note that OFF
only disables the certificate path: Phase 1 self-attestation still returns
`packed` for ES256 credentials.

6. `attestation.h` includes `attestation_cert.h` only when the macro is set.

Acceptance: full attestation response contains `fmt: packed`, `alg: -7`,
`sig`, `x5c`; the signature verifies with the leaf public key; the leaf
verifies to the dev CA; the AAGUID extension matches GetInfo and authData;
both ES256 and EdDSA credentials produce a valid statement.

### Phase 3 — Independent verification (1 day)

Do this before touching Daybreak.

1. Use `scripts/fido2/verify_attestation.py` (implemented) for the raw CTAP2
   check; pass `--pin` if the handset has a PIN configured and `--ca` for the
   chain:

   - INIT and GetInfo; assert `info[3] == kAaguid`.
   - MakeCredential with a fresh challenge.
   - Parse the response: `fmt == "packed"`, `attStmt["alg"] == -7`,
     non-empty `sig` and `x5c`.
   - Verify `sig` over `authData || clientDataHash` with the leaf public key
     using `cryptography`.
   - Verify the leaf against `dev-ca.pem` with `openssl verify` or
     `cryptography`.
   - Assert the authData AAGUID equals GetInfo's AAGUID.
   - Repeat after a service restart and with a resident credential.

   The existing `MacHid` + CBOR helpers already do the transport and
   `exercise()` already parses a MakeCredential response; the new work is
   attestation parsing and the ClientPIN token flow (protocol 1:
   getKeyAgreement → ECDH → SHA-256 → AES-256-CBC PIN block → getPINToken →
   HMAC-SHA256 token over clientDataHash). Reference implementation:
   `pin.h`.

2. Add `scripts/fido2/decode_attestation.py` that accepts a base64 or hex
   `attestationObject` copied from a browser console and prints `fmt`, `alg`,
   certificate count, AAGUID and signature length. This avoids writing a CBOR
   decoder in JavaScript.

3. Browser capture, in this order:

   - Chrome: `navigator.credentials.create({ publicKey: { ...,
     attestation: "direct" } })`; decode `response.attestationObject`.
   - Safari: same page, same options. Safari has historically returned `none`
     or stripped attestation even when `direct` was requested; record the
     actual `fmt` rather than assuming.
   - If Safari returns `none` but Chrome returns `packed`, the authenticator
     is correct and the difference is client policy. Do not change the
     authenticator to chase Safari.

Acceptance: captured evidence shows `fmt: packed`, a verifying signature and
the expected AAGUID from an independent CTAP2 client; browser results are
recorded separately.

### Phase 4 — Move identity material into the seed capability (1–2 days)

This removes the per-device userland header and is the version suitable for
the normal build.

1. Extend the ABI in `shared/include/midix/abi/protocols/fido2.h`:

```cpp
constexpr mx_u32_t MX_FIDO2_SEED_VERSION = 2;

struct MxFido2Seed {
    mx_u32_t version;
    mx_u32_t provisioned;
    mx_u8_t bytes[32];
    mx_u8_t aaguid[16];
    mx_u8_t attestation_public_key[64];
    mx_u16_t attestation_cert_size;
    mx_u16_t reserved;
    mx_u8_t attestation_cert[1024];
};
```

Size ≈ 1148 bytes, well inside the 4096-byte capability page. Keep accepting
version 1 seeds (zero certificate) so existing images still boot.

2. `scripts/fido2/seed.py --header` gains `--aaguid`,
   `--attestation-cert` and `--attestation-public-key`, and emits version 2
   when a cert is supplied, version 1 otherwise. It must reject a cert larger
   than 1024 bytes and a public key that does not match the cert.

3. `main.cc` maps the seed page, copies the master, AAGUID, public key and
   certificate into the `Authenticator`/`Runtime` arena **before**
   `sys_unmap_page(seed_va)`. The existing code unmaps the seed immediately
   after copying the master; a pointer into the seed page would dangle. Copy,
   do not retain the pointer.

4. `Authenticator` gains:

```cpp
mx_u8_t attestation_cert[1024]{};
mx_u32_t attestation_cert_size = 0;
mx_u8_t attestation_public_key[64]{};
```

`prepare_attestation()` checks `attestation_cert_size != 0`, derives the key,
and compares against the seed-provided public key (not a compile-time
constant). `attestation.h` keeps `kAaguid` as the model constant and asserts
the seed AAGUID matches it; a mismatch disables certificate attestation and
logs.

5. Remove the generated `attestation_cert.h` include from the userland build.
   `mochi_system` can then be built once for all devices; only the kernel
   image carries the per-device leaf.

Acceptance: a version 2 seed header round-trips through `seed.py`; the service
copies the cert before unmapping; a v1 seed still boots and reports
`packed/self`; a mismatched cert falls back to self-attestation.

### Phase 5 — Daybreak experiment (0.5 day)

Only after Phase 3 passes. Use a fresh registration; an existing passkey
cannot gain attestation retroactively.

1. Remove the old Ame passkey for the test account (or use a fresh account).
2. Register with `attestation: "direct"` and capture:
   - whether `create()` resolves;
   - `fmt` and `x5c` presence from the decoded attestation object;
   - the page's exact error text.
3. Record the result in `docs/FIDO2_DAYBREAK.md`.

Interpretation:

- `create()` succeeds, page rejects → the remaining gap is service policy /
  authenticator allowlisting, not CTAP encoding.
- `create()` fails with `NotAllowedError` → client/transport/presence/PIN
  issue; return to the existing diagnostics, not attestation.
- `create()` succeeds and the page accepts → the experiment answered the
  question; document whether it was the AAGUID, the cert chain, or both.

## 5. Code-level change list

| File | Change | Phase |
|---|---|---|
| `userland/usb/functions/fido2/attestation.h` | New: AAGUID, cert include, feature flags | 0/2 |
| `userland/usb/functions/fido2/authenticator.h` | AAGUID in `info()`/authData; `prepare_attestation()`; packed `attStmt`; stack-reusing message buffer | 1/2 |
| `userland/usb/functions/fido2/main.cc` | Call `prepare_attestation()`, log format; Phase 4 copy cert before unmap | 2/4 |
| `userland/usb/functions/fido2/store.h` | No change — intentionally avoids a store migration | — |
| `userland/usb/functions/fido2/fs_store.h` | No change | — |
| `userland/CMakeLists.txt` | Attestation option, generated header, include path, compile definition | 2 |
| `scripts/fido2/attestation.py` | New: CA init, leaf issue, C++ header emission, cert verification | 2 |
| `scripts/fido2/verify_attestation.py` | New: independent CTAP2 attestation/PIN verifier | 3 |
| `scripts/fido2/decode_attestation.py` | New: decode a browser-captured attestation object | 3 |
| `tests/fido2_probe.cc` | Packed/AAGUID assertions; print cert/sig/authData/clientHash hex | 1/2 |
| `tests/test_fido2.py` | Compile with a fixture cert; OpenSSL verification; cert profile and derivation tests | 2 |
| `shared/include/midix/abi/protocols/fido2.h` | `MxFido2Seed` v2 | 4 |
| `scripts/fido2/seed.py` | Emit v2 header with cert/public key | 4 |
| `docs/FIDO2_DAYBREAK.md` | Update evidence and Daybreak outcome | 3/5 |
| `.gitignore` | `secrets/`, generated `attestation_cert.h` / `attestation_leaf.der` | 2 |

No changes are required to `hid.h`, `runtime.h`, `pin.h`, `cbor.h`,
`p256.h`, `crypto.h`, `store.h` or `fs_store.h` for Phases 1–3.

## 6. Certificate profile

Leaf:

| Field | Value |
|---|---|
| Version | 3 |
| Subject | `C=GB`, `O=Ame`, `OU=FIDO2`, `CN=Ame FIDO2 Attestation` |
| Issuer | Ame FIDO2 development CA |
| Public key | P-256, derived from master seed domain 5 |
| Basic Constraints | `CA=false`, critical |
| Key Usage | `digitalSignature`, critical |
| Subject Key Identifier | SHA-1 of the public key, non-critical |
| Authority Key Identifier | CA public key, non-critical |
| AAGUID extension | `1.3.6.1.4.1.45724.1.1.4`, value `04 10 <16-byte AAGUID>`, non-critical |
| Signature | `ecdsa-with-SHA256` by the CA |
| Validity | 2020-01-01 to 2046-01-01 (avoid handset clock skew) |
| Serial | random positive integer |

CA:

| Field | Value |
|---|---|
| Subject | `C=GB`, `O=Ame`, `OU=FIDO2`, `CN=Ame FIDO2 Development CA` |
| Basic Constraints | `CA=true`, `path_length=0`, critical |
| Key Usage | `keyCertSign`, `cRLSign`, `digitalSignature`, critical |
| Public key | P-256 |
| Signature | self-signed `ecdsa-with-SHA256` |

The AAGUID extension value is the DER encoding of an OCTET STRING containing
the 16-byte AAGUID. With `cryptography`:

```python
x509.UnrecognizedExtension(
    x509.ObjectIdentifier("1.3.6.1.4.1.45724.1.1.4"),
    b"\x04\x10" + aaguid,
)
```

The `x5c` array carries the leaf DER only. Do not include the root: the
verifier supplies it out of band. If an intermediate is introduced later,
append it after the leaf and before the root.

## 7. Test plan

### 7.1 Host unit tests (`tests/fido2_probe.cc`)

Add a `registration_attestation()` fixture that:

- provisions the fixture seed (`{1..8}`) and calls `prepare_attestation()`;
- registers an ES256 credential (`algorithm = -7`);
- asserts `fmt == "packed"`, `alg == -7`, `sig` non-empty, `x5c` count 1;
- asserts authData AAGUID equals `kAaguid` and GetInfo AAGUID;
- prints `attestation.authdata`, `attestation.clienthash`,
  `attestation.sig`, `attestation.cert` as hex;
- flips a byte in authData and confirms verification fails.

Keep the existing EdDSA fixture. With attestation disabled it must still
return `none`; with the certificate build it must return `packed` with `x5c`.

### 7.2 Python/OpenSSL tests (`tests/test_fido2.py`)

- Generate a fixture CA and leaf with the fixture seed into a temp directory.
- Compile the probe with `-DAME_FIDO2_ATTESTATION=1` and the generated
  include directory.
- Write the printed authData+clientHash, signature and certificate to files.
- Verify with OpenSSL, matching the existing ES256 pattern:

```sh
openssl x509 -inform DER -in leaf.der -pubkey -noout > leaf.pub.pem
openssl dgst -sha256 -verify leaf.pub.pem -signature sig.der message.bin
openssl verify -CAfile ca.pem leaf.pem
```

- Assert the AAGUID extension OID and `0410<AAGUID>` value with
  `openssl x509 -text` or `cryptography` (the build venv has
  `cryptography==50.0.1`).
- Assert `Basic Constraints: CA:FALSE`, the subject fields, and the leaf
  public key matching `attestation.py`'s derived public key.
- Cross-check the Python derivation against a probe-printed
  `attestation.public` for the fixture seed; this catches domain or
  length-encoding drift between C++ and Python.
- Negative: tampered authData fails; tampered clientDataHash fails; a cert
  with the wrong AAGUID extension is rejected by `attestation.py verify`.

### 7.3 CTAP2 over USB (`scripts/fido2/host.py --attestation`)

- GetInfo: AAGUID is non-zero and matches `kAaguid`.
- MakeCredential: `fmt: packed`, `alg: -7`, `sig`, `x5c`.
- Verify the signature and chain independently.
- Restart the service and repeat; a fresh credential must still attest.
- Resident credential: the attestation statement is present and the resident
  record still saves.
- PIN configured: the ClientPIN token flow completes; PIN retry count is
  unchanged after a successful registration.

### 7.4 Browser capture

- Chrome `attestation: "direct"` → decode `attestationObject`; expect
  `fmt: packed`, `x5c` present.
- Safari `attestation: "direct"` → record the actual `fmt`; do not assume it
  matches Chrome.
- If Safari returns `none`, preserve the raw object and browser version in
  `docs/FIDO2_DAYBREAK.md` as a client-policy observation.

### 7.5 Regression matrix

| Build | Credential | Expected `fmt` | `x5c` | Signing key |
|---|---|---|---|---|
| no cert | ES256 | `packed` | no | credential |
| no cert | EdDSA | `none` | no | — |
| cert | ES256 | `packed` | yes | attestation |
| cert | EdDSA | `packed` | yes | attestation |
| cert mismatch | ES256 | `packed` | no | credential (fallback) |
| test build, packed disabled | any | `none` | no | — |

The last row is a regression-only build (for example a test macro that forces
the old path); it is not a supported device configuration.

Also re-run the existing assertion, PIN, resident-credential, exclusion,
presence-timeout and transport tests. The store layout is unchanged, so
existing credentials must continue to assert.

## 8. Risks and mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| Safari strips attestation | No browser-visible `x5c` even though CTAP is correct | Verify with Chrome and a raw CTAP2 client; treat Safari as client policy, not an authenticator bug |
| Daybreak rejects a dev CA | Experiment cannot establish eligibility | Separate "implementation correct" from "trusted"; use the §9 gate and the support question in `docs/FIDO2_DAYBREAK.md` |
| Extra crypto overflows the 4 KB service stack | Crash during registration | Reuse the `auth` tail for `authData || clientHash`; reuse the wiped `seed` buffer for the attestation key; one 72-byte signature buffer; run ASan/UBSan probe |
| CBOR map keys out of canonical order | Browser rejects the response | Write `alg`, `sig`, `x5c` in that order (all length 3); unit-test the parser round-trip |
| Cert/seed drift | Signature verifies but cert public key does not match | Startup public-key comparison; fall back to self-attestation and log |
| Cert larger than budget | `0x7F` or truncated response | `static_assert <= 1024`; `Writer::good` check; keep the leaf profile minimal |
| Per-device leaf privacy | Cross-site tracking | Phase 2 is dev-only; decide model/batch/TEE model before wider use |
| AAGUID changed later | MDS/metadata mismatch | Generate once; document in `attestation.h`; never derive per build |
| Store/ABI break | Lost resident credentials or PIN state | Phases 1–3 do not touch `Stored` or `SealedStore`; Phase 4 only extends the boot seed struct with v1 compatibility |
| Handset clock skew | Verifier rejects cert validity | Long validity window with `notBefore` in the past; the authenticator does not check time |
| Signing failure | Malformed or unsigned statement | Sign before writing the response; on failure return `0x7F`; never emit empty `sig` |

## 9. Daybreak decision gate

The code change can prove that Ame produces a valid packed attestation
statement. It cannot prove that OpenAI will trust it.

What the implementation establishes:

- Ame has a stable model AAGUID.
- The attestation statement is signed by a key the certificate certifies.
- The certificate chain is structurally valid and the AAGUID extension
  matches authData.
- A relying party can verify all of the above without trusting the
  authenticator's self-report.

What it does not establish:

- That the development CA is trusted by OpenAI.
- That Ame is FIDO certified or listed in the FIDO Metadata Service.
- That the attestation key is hardware-backed or non-exportable.
- That the AAGUID is on any allowlist.

Likely outcomes:

- If Daybreak accepts the registration, the missing piece was attestation
  format/provenance and the next work is production key protection and
  metadata.
- If Daybreak rejects it after a verified `packed`/`x5c` response, the
  remaining gap is trust policy (certification, MDS, AAGUID allowlist, or
  hardware key protection). Do not iterate on the encoder in that case.

Explicitly out of scope: copying another vendor's AAGUID, using a leaked
vendor attestation key, or otherwise impersonating a certified authenticator.
That is misrepresentation, not interoperability work, and it would not be a
valid security result.

## 10. Definition of done

- [ ] `authenticatorGetInfo` returns Ame's non-zero AAGUID.
- [ ] authData carries the same AAGUID for every registration.
- [ ] ES256 MakeCredential returns `fmt: packed`, `alg: -7`, `sig`.
- [ ] Certificate builds return `x5c` with a leaf that verifies to the dev CA.
- [ ] The signature verifies over `authData || clientDataHash` with the leaf
      public key.
- [ ] The AAGUID extension matches GetInfo, authData and the model constant.
- [ ] ES256 and EdDSA credentials both attest correctly with a cert.
- [ ] EdDSA falls back to `none` only when no cert is configured.
- [ ] Assertions, PIN, resident credentials, exclusion and presence gating are
      unchanged.
- [ ] `python3 tests/test_fido2.py -v` passes, including the new attestation
      cases.
- [ ] `scripts/fido2/host.py --attestation` passes on the handset.
- [ ] Browser captures are recorded in `docs/FIDO2_DAYBREAK.md`.
- [ ] A Daybreak attempt is documented with the exact outcome, or explicitly
      deferred with the reason.

## Appendix A — CBOR layouts

Full attestation response (ES256 credential):

```text
A3                                      map(3)
  01 66 7061636b6564                    "packed"
  02 59 00C5 <197 bytes>                authData
  03 A3                                 attStmt map(3)
     63 616c67 26                       "alg": -7
     63 736967 58 47 <71 bytes>         "sig": DER ECDSA
     63 783563 81 59 01F4 <500 bytes>   "x5c": [leaf DER]
```

Self-attestation omits `x5c` and uses `attStmt map(2)` with `alg` and `sig`.

authData (ES256, 197 bytes):

```text
0..31    rpIdHash
32       flags: 0x41 (UP + AT), 0x45 when UV is set
33..36   signCount = 0 (unsupported)
37..52   AAGUID (16)
53..54   credentialIdLength = 65
55..119  credentialId
120..196 COSE_Key (map 5: kty 2, alg -7, crv 1, x, y)
```

authData (EdDSA, 162 bytes) is identical through offset 119, then a COSE map 4
with kty 1, alg -8, crv 6 and the 32-byte public key.

## Appendix B — Derivation and certificate commands

Generate the AAGUID once:

```sh
uuidgen
```

Create the development CA (keep `secrets/` out of git):

```sh
mkdir -p secrets
.venv/bin/python scripts/fido2/attestation.py init-ca \
  --key secrets/ame-fido2-dev-ca.key \
  --cert secrets/ame-fido2-dev-ca.pem
```

Issue a leaf for a device seed:

```sh
.venv/bin/python scripts/fido2/attestation.py issue \
  --seed secrets/ame-fido2-seed.bin \
  --aaguid-header userland/usb/functions/fido2/attestation.h \
  --ca-key secrets/ame-fido2-dev-ca.key \
  --ca-cert secrets/ame-fido2-dev-ca.pem \
  --out-header out/qemu-dev/generated/attestation_cert.h \
  --out-cert out/qemu-dev/generated/attestation_leaf.der
```

Configure a device build:

```sh
cmake --preset qemu-dev \
  -DAME_FIDO2_ATTESTATION=ON \
  -DAME_FIDO2_SEED_FILE="$PWD/secrets/ame-fido2-seed.bin" \
  -DAME_FIDO2_CA_KEY="$PWD/secrets/ame-fido2-dev-ca.key" \
  -DAME_FIDO2_CA_CERT="$PWD/secrets/ame-fido2-dev-ca.pem"
```

Verify the leaf independently:

```sh
openssl x509 -inform DER -in out/qemu-dev/generated/attestation_leaf.der \
  -text -noout
openssl verify -CAfile secrets/ame-fido2-dev-ca.pem \
  <(openssl x509 -inform DER -in out/qemu-dev/generated/attestation_leaf.der)
```

Verify the live CTAP2 response (add `--pin` if the handset has a PIN):

```sh
.venv/bin/python scripts/fido2/verify_attestation.py \
  --pin "$AME_FIDO2_PIN" --ca secrets/ame-fido2-dev-ca.pem
```

Decode a browser-captured `attestationObject`:

```sh
.venv/bin/python scripts/fido2/decode_attestation.py '<base64-or-hex>'
```

## Appendix C — Open questions

1. Does Daybreak require FIDO certification/MDS, or will any verifiable
   attestation chain do? The public guidance does not say; the support
   question in `docs/FIDO2_DAYBREAK.md` is the right next step.
2. Does Safari forward packed attestation for a USB security key on the
   current iOS/macOS version? Chrome is the comparison.
3. Should Ame advertise `attestationFormats` (GetInfo key `0x16`) and/or
   `FIDO_2_1`? Some CTAP 2.1 clients may use it to decide whether to request
   attestation; test with and without before changing the versions list.
4. Model-level vs per-device certificate: decide before any distribution
   beyond the test handset.
5. TrustedCore integration for a non-exportable attestation key: separate
   project; do not block the software attestation experiment on it.
