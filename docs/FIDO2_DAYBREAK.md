# Ame FIDO2 and Daybreak investigation

## Attestation implementation status (17 October 2026)

Ame now emits `packed` attestation instead of `none`: self-attestation for
ES256 when no certificate is configured, and certificate-backed `packed` with
a development-CA leaf when built with `AME_FIDO2_ATTESTATION=ON`. The host
probe verifies the signature, chain and AAGUID, and the service links with the
generated certificate. Hardware CTAP2 verification, browser capture and the
Daybreak trial remain to be run. See
[`FIDO2_ATTESTATION_PLAN.md`](FIDO2_ATTESTATION_PLAN.md).

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

## Confirmed implementation findings

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

## Evidence needed from the failing browser attempt

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

## If Daybreak requires verified authenticator provenance

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

## Support question (draft; not sent)

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

## Sources

- [OpenAI Daybreak troubleshooting](https://help.openai.com/en/articles/20001259-openai-daybreak-common-issues-and-troubleshooting)
- [OpenAI Daybreak setup overview](https://help.openai.com/en/articles/20001258-openai-daybreak-trusted-access-for-cyber-overview)
- [WebAuthn attestation types](https://www.w3.org/TR/webauthn-3/#sctn-attestation-types)
- [FIDO attestation and metadata](https://fidoalliance.org/fido-technotes-the-truth-about-attestation/)
