# Phase I (verified), J (framebuffer/TTY migration) and K (lifecycle hardening)

Base `41c1aaa`. Commits `28bb0d5` (J), `7ab0e0f` (K).

## Phase I — shared staging framebuffer (already landed, re-verified)

Unchanged from `654210c`; re-verified green after J and K (`gpud`, `kms-display` rows).

## Phase J — framebuffer / TTY migration onto the appliance

- A Mochi-facing GPU service (`drv.gpu.linux` role, spec §15.1) is published by `dango.manager`, which
  already owns the appliance VM, the Dango channel and the `gpu.*` caps: it registers the shared staging
  window and accepts `PRESENT`. Its protocol is a small explicit ABI
  (`shared/include/midix/abi/protocols/gpu_service.h`) — not raw DRM and not the whole GPU opcode space.
  There is deliberately **no separate `drv.gpu.linux` process** in the bootstrap (the channel caps cannot
  yet be passed between native services); the manifest documents this.
- `io.framebuffer` (`userland/drivers/framebuffer/main.cc`) arms a GPU backend: it handshakes
  `INFO → REGISTER_STAGING → Live` without blocking startup and translates `FB_PRESENT` into a present
  request. The ramfb path is unchanged when the appliance is absent, so the default boot and `tty.fb0`
  still work; a missing/dead appliance degrades to ramfb instead of failing the service.
- The bootstrap mirrors the framebuffer into the shared staging window rather than re-pointing
  `io.framebuffer`'s render target at it: a native process cannot map `MX_VM_MEMORY`, and the arena cap is
  64 pages. The copy is exactly the bounded bootstrap copy the spec permits and Phase M removes.
- Appliance boot shows `io.framebuffer: gpu backend armed`, `gpu service: …`, `tty.fb0: ready`,
  `tty.fb0: shell terminal …` — the TTY reaches the shell with the framebuffer presented through the
  appliance. No native service contains virtio-gpu register knowledge.

## Phase K — lifecycle hardening

- **boot generation** — a kernel counter advances on every appliance channel (re)establishment, is
  written into the guest descriptor (the old code put the channel id there — a latent field bug, fixed),
  and gates `map`/`stage`/`notify`/`endpoint`/HVC with `E_NOTFOUND` on a stale reference. A second
  channel on a VM is a documented reconnect.
- **back-pressure** — a full producer ring returns `E_BUSY`; no spin, no silent drop. The fixed ring
  capacity bounds outstanding operations.
- **failure propagation / DMA-safe teardown** — appliance death detaches the channel from the dying vCPU
  (clearing its vGIC pending bit) and tears the assignment down into the **faulting** domain before any
  page is returned; a quarantined IOMMU domain is never handed out again.
- **tests** — `tests/kernel/lifecycle.cc` gained stale-generation rejection, full-ring back-pressure,
  reset/reconnect and destroy/recreate cases; `tests/kernel/iommu_lifecycle_probe.cc` +
  `scripts/test/lifecycle.py` prove a cleanly revoked domain is reusable and a quarantined one never is.
  Opt-in gate row `AME_VERIFY_LIFECYCLE=1`.

## Verification (Clang 23.1.1 / CMake 4.4.0 / QEMU 11.1.1)

- `scripts/verify.sh` — **all checks passed** (default boot unchanged).
- `dango-lifecycle` (`AME_VERIFY_LIFECYCLE=1`) — `{"success": true, "reason": "iommu lifecycle probe ok"}`.
- `gpud`, `gpu-appliance`, `kms-display`, `dango-channel` — all green after J and K.

## Notes / limits

- The appliance acceptance still has no dedicated screendump row for the *tty* frame (the `gpud` row
  asserts the bootstrap pattern); adding one is a small harness follow-up.
- The SMMU assignment death path needs the opt-in testdev topology (`AME_VERIFY_ASSIGN`); its
  quarantine/lifecycle logic is covered host-side.
- The latent `host_irq=0` MSI route (INTx-only profile) was left as-is, out of scope.
