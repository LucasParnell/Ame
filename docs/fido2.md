# FIDO2 implementation and attestation

The implementation emits packed attestation and has host signature/chain verification. Phone GetInfo/presence/PIN observations, full certificate-backed registration and relying-party trust are separate evidence boundaries. Completed phase checklists and the obsolete zero-AAGUID baseline have been removed. The reference retains design rationale, protocol layouts, certificate profile and acceptance work.

- [Ame FIDO2 and Daybreak investigation](#fido2_daybreak)
- [Ame FIDO2 attestation (direct) — implementation plan](#fido2_attestation_plan)

<a id="fido2_daybreak"></a>
## Ame FIDO2 and Daybreak investigation

<a id="fido2_daybreak-ame-fido2-and-daybreak-investigation"></a>

<a id="fido2_daybreak-attestation-implementation-status-17-october-2026"></a>
### Attestation implementation status (17 October 2026)

Ame now emits `packed` attestation instead of `none`: self-attestation for
ES256 when no certificate is configured, and certificate-backed `packed` with
a development-CA leaf when built with `AME_FIDO2_ATTESTATION=ON`. The host
probe verifies the signature, chain and AAGUID, and the service links with the
generated certificate. Hardware CTAP2 verification, browser capture and the
Daybreak trial remain to be run. See
[`FIDO2_ATTESTATION_PLAN.md`](fido2.md#fido2_attestation_plan).

v48 was flashed on 17 October 2026 and booted. `authenticatorGetInfo` reports
AAGUID `dc0cdc6381704f9d83fc07270e413bd9`, the service log reports
`attestation: packed/x5c`, and the configured PIN was preserved (8 retries,
0 attempts). The remaining check is a MakeCredential with a PIN/UV prompt
(Safari or `verify_attestation.py --pin`), then the Daybreak trial.

Checked 17 October 2026. Safari registration/signing has worked on the connected
Anne phone with v45, but the user reports intermittent prompts and approval.
The exact Daybreak error is: “You or your device disallowed the passkey creation.
Please try again.” This does not establish an attestation or hardware eligibility
rejection; it can occur while the browser is creating a credential.

V46 was subsequently flashed with explicit user approval. The handset boot tag,
INIT/GetInfo and 1024-byte PING were verified; GetInfo still reports a configured
PIN. The subsequent browser attempt reached ClientPIN getRetries (1),
getKeyAgreement (2), getPINToken (5), all status 0, followed by MakeCredential
and a fresh physical approval with status 0. The user then confirmed that the
page still asks for a supported hardware key. Thus fixing the transport/presence
issue has not established Daybreak hardware eligibility. A successful CTAP
response alone does not prove the browser or server accepted the credential.

<a id="fido2_daybreak-confirmed-implementation-findings"></a>
### Confirmed implementation findings

- V47 keeps the PIN key-agreement key stable across repeated getKeyAgreement
  discovery requests. V46 generated a new key on every request; the live failure
  log contains two such requests 380 ms apart. A second discovery could therefore
  invalidate the shared secret a browser already held while collecting its PIN.
  A regression checking repeated discovery failed before this fix. The key still
  changes after an invalid PIN or a fresh service incarnation, and persistence
  failure still prevents exposing a new key. V47 was flashed after explicit user
  approval. On-device checks verified the same valid P-256 public point across
  three discovery queries from two channels, configured PIN, eight remaining
  retries, and successful INIT/GetInfo/PING. These checks made no PIN attempts.
  Safari registration after this change remains to be retried.
- `Hid::tick` discarded a fresh physical press if a keepalive reply was queued.
  The physical sampler consumes that edge, so holding the button could not retry
  it. V46 retains an approval for the active transaction until the reply drains;
  cancellation clears it. The regression failed before the fix and passes after.
- CTAP 2.0 uses an empty PIN authentication parameter for physical key selection.
  Successful selection returns `PIN_NOT_SET` (0x35) or `PIN_INVALID` (0x31), as
  appropriate. V45 displayed those expected selection results as “rejected.”
  V46 identifies selection separately, without changing its protocol response.
- V45's diagnostic printed an old CTAP status on subsequent INIT packets. V46
  logs each actual CTAP transaction, command and ClientPIN subcommand, without
  logging PINs, token data, account IDs, credential IDs or challenges.
- Ame supports USB CTAPHID, ES256, PIN protocol 1, physical user presence,
  discoverable credentials and PIN-authenticated user verification. Its
  authenticator data leaves the backup eligibility/state flags clear.
- GetInfo and registration carry an all-zero AAGUID; registration uses `none`
  attestation with an empty statement. Ame has no manufacturer attestation key,
  certificate chain or trusted model metadata. Its host-provisioned master seed
  is compiled into the private kernel image; it is not a non-exportable TEE key.

<a id="fido2_daybreak-evidence-needed-from-the-failing-browser-attempt"></a>
### Evidence needed from the failing browser attempt

The supplied browser diagnostic now establishes `create`, `attestation: direct`,
`cross-platform`, resident key required, user verification required, ES256/RS256
offered, one excluded credential, ending with `NotAllowedError`. This outcome
does not establish a server attestation-policy rejection: the native browser
operation did not resolve successfully. The corresponding phone snapshot stopped
after key selection, getRetries and two getKeyAgreement requests; it showed no
getPINToken or completed MakeCredential for that attempt.

In Safari's Web Inspector console on the page actually performing registration,
run the reviewed contents of
[`browser-diagnostics.js`](../scripts/fido2/browser-diagnostics.js), then retry
the Add action once. The script observes the existing request; it does not
change options, submit extra requests or send diagnostics elsewhere.

`ameFidoDiagnostics.records` contains only registration requirements, browser
outcome, attachment, transports, presence/verification/backup flags and AAGUID.
It excludes challenges, user details, credential IDs, public keys, signatures,
PINs and tokens. Reload the page or call `ameFidoDiagnostics.restore()` to remove
the hook. Hooks must run in the frame/origin that invokes WebAuthn.

- Browser `NotAllowedError`, with no successful registration: investigate the
  transport/presence/selection/PIN path and the requested browser options first.
- Browser success with `cross-platform`, `usb`, `uv=true`, followed by Daybreak
  rejection: inspect the page's specific eligibility message. That separates a
  successful authenticator exchange from service policy/account configuration.
- An existing passkey cannot gain new registration attestation via firmware
  updates. Any eventual attestation implementation needs a fresh registration.

<a id="fido2_daybreak-if-daybreak-requires-verified-authenticator-provenance"></a>
### If Daybreak requires verified authenticator provenance

The public OpenAI guidance requires a qualifying physical FIDO2 key and explicitly
excludes some FIDO2 models. It does not publish its model allowlist, certificate
trust roots, AAGUID classification logic or attestation verification algorithm.
We cannot conclude that any particular attestation change will qualify Ame.

The next discriminating capture is the browser diagnostics described above:
confirm that Safari resolves `create()` successfully, reports `cross-platform`
and `usb`, and exposes the intended verification flags. If these are correct,
OpenAI must clarify how its service qualifies this custom authenticator. The
public guidance does not establish that FIDO certification/MDS inclusion alone,
self attestation, or any particular manufacturer chain is sufficient.

A genuine attestation implementation would need an Ame model AAGUID, a properly
provisioned attestation key/certificate chain, a supported attestation format
and accurate security metadata. Hardware-backed credential/attestation operations
would require additional trusted-firmware integration on Anne. Daybreak would
also have to accept that model or its trust chain. A new GUID or self-signed
certificate alone does not establish trust. Self attestation proves possession
of the credential key, not the manufacturer's hardware security properties.

<a id="fido2_daybreak-support-question-draft-not-sent"></a>
### Support question (draft; not sent)

> I am testing a custom USB CTAP2/FIDO2 authenticator implemented by Ame on a
> Huawei ANE-LX1. It supports ES256, PIN protocol 1, physical user presence and
> resident credentials. The current firmware returns `fmt: none`, an empty
> attestation statement and an all-zero AAGUID. Its keys currently use software
> protection, not a TEE-backed non-exportable key. After a successful PIN-token
> exchange and MakeCredential response, Daybreak still asks for a supported
> hardware security key. Can custom authenticators qualify? Does qualification
> require an approved AAGUID, a validated attestation certificate chain, FIDO
> Metadata Service registration, particular key-protection properties, or a
> combination? Is there a published onboarding path for a custom model?

<a id="fido2_daybreak-sources"></a>
### Sources

- [OpenAI Daybreak troubleshooting](https://help.openai.com/en/articles/20001259-openai-daybreak-common-issues-and-troubleshooting)
- [OpenAI Daybreak setup overview](https://help.openai.com/en/articles/20001258-openai-daybreak-trusted-access-for-cyber-overview)
- [WebAuthn attestation types](https://www.w3.org/TR/webauthn-3/#sctn-attestation-types)
- [FIDO attestation and metadata](https://fidoalliance.org/fido-technotes-the-truth-about-attestation/)

<a id="fido2_attestation_plan"></a>
## Attestation reference and acceptance

<a id="fido2_attestation_plan-ame-fido2-attestation-direct-implementation-plan"></a>

Implementation/reference checkpoint, 17 October 2026.
Target: `usb.fn.fido2`; see the implementation status below.
Goal: make `authenticatorMakeCredential` return a cryptographically valid
`packed` attestation statement carrying Ame's own AAGUID — first self-attested,
then certificate-backed — and verify it independently of any browser.
Non-goal: making OpenAI Daybreak trust the result. That is a separate trust
policy question, covered by the decision gate in §9.

<a id="fido2_attestation_plan-implementation-status-17-october-2026"></a>
### Implementation status (17 October 2026)

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

<a id="fido2_attestation_plan-2-target-behaviour"></a>
### Attestation response shape

The certificate-backed ES256 registration response has this shape:

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

<a id="fido2_attestation_plan-3-design-decisions"></a>
### 3. Design decisions

<a id="fido2_attestation_plan-31-packed-not-fido-u2ftpmandroid-key"></a>
#### 3.1 Packed, not fido-u2f/tpm/android-key

`packed` is the generic WebAuthn attestation format for standalone
authenticators. It supports the existing ES256 credential path, and a
dedicated ES256 attestation key can attest both ES256 and EdDSA credentials.
`fido-u2f` would force a U2F-shaped certificate and signature format;
`tpm` and `android-key` require platform facilities Ame does not have.
`none` is the current state and carries no provenance.

<a id="fido2_attestation_plan-32-one-aaguid-generated-once"></a>
#### 3.2 One AAGUID, generated once

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

<a id="fido2_attestation_plan-33-attestation-key-derived-from-the-master-seed"></a>
#### 3.3 Attestation key derived from the master seed

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

<a id="fido2_attestation_plan-34-certificate-backed-statement"></a>
#### 3.4 Certificate-backed statement

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

<a id="fido2_attestation_plan-35-certificate-delivery"></a>
#### 3.5 Certificate delivery

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

<a id="fido2_attestation_plan-36-privacy-model-cert-vs-per-device-cert"></a>
#### 3.6 Privacy: model cert vs per-device cert

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

<a id="fido2_attestation_plan-37-failure-behaviour"></a>
#### 3.7 Failure behaviour

- Attestation signing failure → CTAP `0x7F`, no partial response.
- Cert/key mismatch at startup → disable certificate attestation; use self
  attestation (ES256) or `none` (EdDSA).
- Response buffer exhaustion → `Writer::good == false`; return `0x7F`.
- Never silently downgrade from certificate-backed to self-attestation or
  `none` without a log marker; downgrade is observable in `ctap.status` / a
  new counter.

<a id="fido2_attestation_plan-6-certificate-profile"></a>
### 6. Certificate profile

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

<a id="fido2_attestation_plan-7-test-plan"></a>
### 7. Test plan

<a id="fido2_attestation_plan-71-host-unit-tests-testsfido2_probecc"></a>
#### 7.1 Host unit tests (`tests/fido2_probe.cc`)

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

<a id="fido2_attestation_plan-72-pythonopenssl-tests-teststest_fido2py"></a>
#### 7.2 Python/OpenSSL tests (`tests/test_fido2.py`)

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

<a id="fido2_attestation_plan-73-ctap2-over-usb-scriptsfido2hostpy---attestation"></a>
#### 7.3 CTAP2 over USB (`scripts/fido2/host.py --attestation`)

- GetInfo: AAGUID is non-zero and matches `kAaguid`.
- MakeCredential: `fmt: packed`, `alg: -7`, `sig`, `x5c`.
- Verify the signature and chain independently.
- Restart the service and repeat; a fresh credential must still attest.
- Resident credential: the attestation statement is present and the resident
  record still saves.
- PIN configured: the ClientPIN token flow completes; PIN retry count is
  unchanged after a successful registration.

<a id="fido2_attestation_plan-74-browser-capture"></a>
#### 7.4 Browser capture

- Chrome `attestation: "direct"` → decode `attestationObject`; expect
  `fmt: packed`, `x5c` present.
- Safari `attestation: "direct"` → record the actual `fmt`; do not assume it
  matches Chrome.
- If Safari returns `none`, preserve the raw object and browser version in
  `docs/fido2.md#fido2_daybreak` as a client-policy observation.

<a id="fido2_attestation_plan-75-regression-matrix"></a>
#### 7.5 Regression matrix

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

<a id="fido2_attestation_plan-8-risks-and-mitigations"></a>
### 8. Risks and mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| Safari strips attestation | No browser-visible `x5c` even though CTAP is correct | Verify with Chrome and a raw CTAP2 client; treat Safari as client policy, not an authenticator bug |
| Daybreak rejects a dev CA | Experiment cannot establish eligibility | Separate "implementation correct" from "trusted"; use the §9 gate and the support question in `docs/fido2.md#fido2_daybreak` |
| Extra crypto overflows the 4 KB service stack | Crash during registration | Reuse the `auth` tail for `authData || clientHash`; reuse the wiped `seed` buffer for the attestation key; one 72-byte signature buffer; run ASan/UBSan probe |
| CBOR map keys out of canonical order | Browser rejects the response | Write `alg`, `sig`, `x5c` in that order (all length 3); unit-test the parser round-trip |
| Cert/seed drift | Signature verifies but cert public key does not match | Startup public-key comparison; fall back to self-attestation and log |
| Cert larger than budget | `0x7F` or truncated response | `static_assert <= 1024`; `Writer::good` check; keep the leaf profile minimal |
| Per-device leaf privacy | Cross-site tracking | Phase 2 is dev-only; decide model/batch/TEE model before wider use |
| AAGUID changed later | MDS/metadata mismatch | Generate once; document in `attestation.h`; never derive per build |
| Store/ABI break | Lost resident credentials or PIN state | Phases 1–3 do not touch `Stored` or `SealedStore`; Phase 4 only extends the boot seed struct with v1 compatibility |
| Handset clock skew | Verifier rejects cert validity | Long validity window with `notBefore` in the past; the authenticator does not check time |
| Signing failure | Malformed or unsigned statement | Sign before writing the response; on failure return `0x7F`; never emit empty `sig` |

<a id="fido2_attestation_plan-9-daybreak-decision-gate"></a>
### 9. Daybreak decision gate

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

<a id="fido2_attestation_plan-appendix-a-cbor-layouts"></a>
### Appendix A — CBOR layouts

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

<a id="fido2_attestation_plan-appendix-b-derivation-and-certificate-commands"></a>
### Appendix B — Derivation and certificate commands

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

<a id="fido2_attestation_plan-appendix-c-open-questions"></a>
### Appendix C — Open questions

1. Does Daybreak require FIDO certification/MDS, or will any verifiable
   attestation chain do? The public guidance does not say; the support
   question in `docs/fido2.md#fido2_daybreak` is the right next step.
2. Does Safari forward packed attestation for a USB security key on the
   current iOS/macOS version? Chrome is the comparison.
3. Should Ame advertise `attestationFormats` (GetInfo key `0x16`) and/or
   `FIDO_2_1`? Some CTAP 2.1 clients may use it to decide whether to request
   attestation; test with and without before changing the versions list.
4. Model-level vs per-device certificate: decide before any distribution
   beyond the test handset.
5. TrustedCore integration for a non-exportable attestation key: separate
   project; do not block the software attestation experiment on it.
