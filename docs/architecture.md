# Ame architecture

Ame is an experimental AArch64 operating system composed of the **Anko**
capability microkernel, **Mochi** native services and libraries, and **Dango**
virtual machines. Linux runs as a managed driver appliance; it does not own the
host's scheduling, native applications, filesystem or display policy.

This is the implementation overview for the working tree reviewed on
24 October 2026. **Midix is the legacy name.** Existing `midix/` include paths,
`mx_*` symbols and `MXI1`/`MXB1` formats retain their spellings for source and ABI
compatibility; they do not name a separate system. This documentation update
does not rename code or change an ABI.

The source references below identify mechanisms. Hardware results are recorded
observations from particular packages, not fresh validation of every local
change. Detailed guides retain dated measurements and investigations; their
older checkpoints do not override the current behavior described here.

## System shape

```text
Native applications and msh
    │ libmochi, filesystem clients, native EGL/GLES and Vulkan clients
    │ shared SPSC rings + capability-scoped doorbells and arenas
    ├── init / nameservice / io.fs / USB services / tty.fb0
    ├── io.framebuffer ────────────────────────────────→ panel
    └── dango.manager
            │ bounded host/guest channel, shared regions, virtual IRQs
            └── Linux appliance → Mesa + Panfrost / experimental PanVK → Mali

Anko: capability lifetime, address spaces, Ressha scheduling, IRQs,
      board mechanisms, stage-2 mappings, EL2 monitor and DMA containment
```

The main implementation boundaries are:

| Component | Responsibility | Source |
|---|---|---|
| Anko | Enforce capabilities, memory and object lifetime; schedule native threads and vCPU runners; provide IPC wakeups and narrow hardware mechanisms | `kernel/` |
| Mochi init | Interpret authenticated service policy, provision channels, launch services, track readiness and supervise generations | `userland/init/` |
| Native services | Discovery, files, USB policy/classes, input, terminal, framebuffer and applications | `userland/` |
| libmochi | Freestanding syscall wrappers, boot entries, shared rings, framing, endpoints and sessions | `userland/lib/` |
| Dango manager | Construct and run appliances, broker GPU requests, coordinate shared buffers and native presentation | `userland/dango/manager/` |
| Linux appliance | Carry the selected Linux driver and Mesa runtime behind the Dango protocol | `linux-artifacts/` |
| Shared contract | Byte layouts, rights, protocol IDs, formats and guest address bindings | `shared/include/midix/` |
| Board layer | Measured hardware definitions, boot handoff and fixed board operations | `boards/` |

Policy belongs in native services and the signed system descriptor where
possible. Anko still contains practical bootstrap and hardware mechanisms:
UART/timer/GIC, PCI discovery, board eMMC/RPMB support, IOMMU assignment, Mali
register mediation and display retirement. The tree is not a claim that every
driver has already moved out of the kernel.

## Current platforms and profiles

Hardware selection (`ANKO_BOARD`) and service selection
(`MOCHI_SYSTEM_PROFILE`) are separate. Running an Anne service profile on the
QEMU board is useful for synthetic tests and does not emulate Hi6250 hardware.
[CMake presets](../CMakePresets.json) provide common combinations; the full
service membership and overrides live in
[`userland/system.json`](../userland/system.json).

| Board / profile | Current scope and evidence |
|---|---|
| `qemu-virt` | Main automated kernel target: direct/disk boot, SMP, native console/TTY, USB input, virtio block, AmeFS, shell and optional Linux GPU/channel tests |
| `huawei-anne` | Minimal P20 Lite recovery/console service profile: nameservice and console |
| `huawei-anne-smp` | Minimal profile plus pinned SMP workers; eight-core native execution recorded on ANE-LX1 |
| `huawei-anne-usb` | Full phone service profile: native USB roles/classes, serial/TTY, apps stream, eMMC/AmeFS, FIDO2, shell and Dango authority/probe |
| `huawei-anne-gpu` | Phone profile with GPU bindings for the Linux Mali appliance and native graphics clients; hardware rendering and direct scanout recorded |
| `dango-hello`, `dango-panfrost` | Reduced native service selections for VM/channel bring-up; guest payload and GPU enablement are separate build inputs |
| `switch-t210` | Built/staged Hekate → ATF → U-Boot route and reduced display/console profile; physical boot acceptance remains unresolved |

Presets normally disable automatic Linux artifact construction. A Dango manager
without a packaged guest can initialize authority/layout and then skip execution.
Neither a successful native boot nor a GPU-disabled channel test proves GPU
execution. x86, a general phone UI and broad hardware support are future work.

## Boot, image integrity and service policy

The live entry is [`kernel/arch/arm64/entry.S`](../kernel/arch/arm64/entry.S),
followed by `kernel_main()`. The legacy `boot/` shim is not the normal built
entry. Boot establishes board memory reservations, address translation and
caches, exceptions, the timer/GIC, capability state and scheduling, then loads
init from the authenticated system bundle. SMP CPUs establish their own entry
and monitor state; the scheduler dispatches EL0 services.

The build pipeline is:

1. Build freestanding static AArch64 ELF services.
2. Convert them to native `MXI1` images with `scripts/mx/mxld.py`.
3. Validate the descriptor, add image digests, sign the `MXM1` manifest, and
   package it with images/resources in `system.mxb` (`MXB1`).
4. Anko validates bundle/object CRC32C, object SHA-256, manifest digest and
   Ed25519 signature against its configured trust store before publishing the
   archive or starting init.
5. Init reads the verified manifest, checks service image identities and
   grants each child its named `MxBootInfo` resources.

CRC detects corruption; it grants no authority. The signed manifest defines
service principals and grants. Release configuration requires external signing
and trust keys and rejects the development key. Rejected bundles do not fall
back to unsigned runtime ELF, tar or embedded-service loading.

QEMU accepts an external bundle with the raw Image or a U-Boot disk package.
The native Anne Image embeds the signed bundle as part of its phone boot image;
embedding does not skip authentication. The bootloader/firmware trust chain is
a separate boundary from Ame's bundle signature checks.

The descriptor defines profiles, service dependencies, restart policies,
affinity, channels, resource aliases, endpoints and named grants. The packer
rejects invalid references, duplicate keys, cycles, invalid geometry and rights,
and oversized tables. Init validates the authenticated policy again into bounded
arrays. There is no JSON parsing on a steady-state IPC path. Generated channel
identifiers and the compatibility `manifest.v0` listing are outputs, not
additional policy inputs. Some board/discovery grants still use explicit init
mechanisms rather than fully generic descriptor interpretation.

Sources: [`boot_archive.cc`](../kernel/init/boot_archive.cc),
[`image_loader.cc`](../kernel/init/image_loader.cc),
[`boot_caps.cc`](../kernel/init/boot_caps.cc),
[`system_descriptor.py`](../scripts/mx/system_descriptor.py).
[ABI and formats](abi.md) carries the programmer-facing details.

## Capabilities, principals and lifetime

Authority is represented by handles in each process's bounded capability table.
A handle identifies a slot and its generation. Lookup checks generation, object
type and requested rights; sharing/duplication must attenuate rights. BootInfo
names describe resources actually granted to a child rather than global names
that imply access.

Capabilities cover processes/threads, rings, doorbells, arenas, MMIO, IRQs,
scheduling contexts, narrow platform control, and VM/memory/channel/device/shared
objects. The authoritative type/right constants are in
[`cap.h`](../shared/include/midix/abi/cap.h). Slot occupancy bitmaps avoid scans
across empty entries during allocation and teardown. Object records retain
references for capabilities, mappings and in-flight operations; final destruction
occurs only after those references are released.

Revoking a handle invalidates that slot's generation and drops its reference.
Revoke-by-target and process cleanup visit live slots. This is not the legacy
architecture's proposed global derivation-tree revocation mechanism: duplicated
handles and live mappings have their own lifetimes. Revoke must not be confused
with proof that a hardware device has stopped reading memory.

Principals supply trusted/sandboxed authorization, weights and optional shared
CPU budgets. Incarnation/generation checks prevent recycled process/principal
IDs from inheriting exits, fairness state or ownership. Human identity is not a
kernel account system.

Sources: [`kernel/cap/`](../kernel/cap/),
[`accounting.cc`](../kernel/sched/accounting.cc),
[`sched_context.cc`](../kernel/sched/sched_context.cc).

## Memory and DMA ownership

Anko has a physical bitmap allocator, heap/slab allocation, per-process TTBR0
address spaces and arena bookkeeping. Native images, stacks, mapped rings and
shared arenas have explicit bounds, permissions and retained owners. Mapping
failure unwinds partial mappings. Address-space activation uses ASIDs; ordinary
activation avoids a global TLB flush, while ASID reuse performs invalidation.

Arenas are explicit shared/pinned page sets with capability-mediated mapping.
CPU memory attributes matter: a write-back CPU arena cannot become safe for a
noncoherent DMA engine merely by adding an ordering barrier. Normal-NC DMA arenas,
cache maintenance, device completion and lifetime retention serve different
purposes. A CPU mapping, a guest stage-2 mapping and permission for a device to
DMA are separate grants.

Native xHCI and assigned PCI paths use verified IOMMU domains. The generic
IOMMU core owns pinned extents, permissions and routes; the ARM SMMUv3 backend
publishes tables, completes invalidations and quarantines faults. The software
backend cannot authorize physical DMA. Unsupported routes fail closed, and
uncertain completion retains backing rather than allowing reuse.

Anne Mali uses a different mechanism: host-built **Mali shadow page tables**,
not an assumed external SMMU. The host validates guest GPU mappings against
owned guest RAM and explicitly registered SharedGpu regions. GPU page-table
storage must remain in ordinary owned guest RAM. CPU command and shader-cache
arenas are excluded from GPU DMA. No supported GPU-to-external-SMMU route has
been established for Hi6250, and hardware rendering is not a complete hostile-
guest containment proof.

Sources: [`kernel/mm/`](../kernel/mm/), [`kernel/iommu/`](../kernel/iommu/),
[`mali_shadow.cc`](../kernel/vm/mali_shadow.cc).
[IOMMU details](iommu-smmuv3.md) and
[Anne GPU routing evidence](../boards/huawei-anne/gpu-isolation.md) cover the two
mechanisms separately.

## IPC: shared data, explicit wakeups

The native transport is a capability-provisioned **SPSC** ring with separate
notification objects. Steady-state publication/consumption can remain entirely
in userspace while both peers are running and mappings are resident. Sleeping,
waking, capability operations and scheduling still cost kernel work. There is
no measured universal cycle-floor guarantee.

The byte contract is [`ring.h`](../shared/include/midix/abi/ring.h): a 256-byte
header with separate 64-byte head, tail, consumer-sleep and producer-space lines;
64-byte slots contain type, length, sequence and 48 inline payload bytes. Orders
4–10 give 16–1,024 slots. Framing can carry larger logical messages, including
packed records; application payload arenas are separate from slot storage.

Producers fill payloads before release-publishing head. Consumers acquire head
before reading and release-publish tail after consumption. Bounded batches
amortize cursor publication and wake coordination. Try-style APIs report full
or empty without hidden blocking. A ring is not a generic concurrent MPSC queue;
services needing multiple clients use separate sessions/provisioned rings.

A receiver arms sleep, rechecks work and uses the doorbell counter handshake to
avoid a lost wake. The producer performs the required sleep-flag atomic operation
and notifies when needed. A doorbell admits one registered waiter; another
waiter can receive busy. Timed waits use absolute monotonic deadlines and bounded
per-CPU timer records; callers always recheck work after notify/expiry.

Same-CPU synchronous request/reply can hand execution to an eligible sleeping
peer and temporarily donate scheduling advantage. Affinity, principal/context
budgets and reply-chain checks still apply. Remote wakes publish retained thread
records to the destination CPU's pending queue and send a coalesced IPI. A wake
cannot publish the thread runnable before its executing CPU has saved the full
exception frame. Cold coordination still uses guards; the entire IPC subsystem
is not lock-free.

Endpoint descriptors fit in one inline payload and bundle handles/protocol
identity. Nameservice and filesystem use bounded per-client SPSC sessions
provisioned by init. This is useful session machinery, not unlimited dynamic
service discovery or arbitrary cross-service endpoint transfer.

Sources: [`kernel/ipc/`](../kernel/ipc/),
[`mochi/ring.h`](../userland/lib/include/mochi/ring.h),
[`mochi/sessions.h`](../userland/lib/include/mochi/sessions.h).
[IPC](ipc.md) retains framing, wait/handoff details and implementation history.

## Scheduling: Ressha / Eki

Each CPU owns one **Eki** station, its runnable queues, dispatch state and
accounting clocks. Remote CPUs publish work rather than modifying its heaps.
Native RT uses trusted fixed-priority FIFO queues. Other working bands share
weighted principal fairness, then weighted thread fairness. Equal keys use
native latency, VM latency, native fair, VM fair, then batch precedence; idle
runs last. A busy latency principal cannot permanently starve fair work simply
by remaining in a higher band.

Principal budgets aggregate across CPUs; scheduling-context budgets are
independent. An aligned atomic budget window commits charge and period rollover
together. Exhausted work remains queued but ineligible until refill. Wake credit
and eligible handoff are bounded advantages, not exemptions from budgets.

Idle stations can request eligible lower-band work. Affinity, pinned/IPC-hot
work and VM stealability constrain migration. Deferred wake publication protects
saved-frame ownership across block/wake races. The ordinary dispatch path avoids
a lifecycle lock; blocking, stealing, binding, retirement and other coordination
still have locks.

Slices and nominal 100 µs IPC/handoff windows are policy targets. The generic
timer checks at roughly 10 ms, so accounting checkpoints can overshoot budgets
and deadlines. There is no hard real-time admission or verified guest-thread
WCET mechanism. Idle waits use timer/events rather than a permanent runnable
poll loop where the service supports deadline waits.

[`scheduler.md`](scheduler.md) gives the precise fairness, migration and lifetime
rules; [`performance.md`](performance.md) separates host/QEMU timings from phone
observations.

## Native service lifecycle and user interaction

Init waits for readiness and honors dependency order. It consumes generation-
tagged exits, restarts affected dependents and applies crash backoff; planned
stops are distinguished from crashes. Nameservice resolves only provisioned
exports and grants endpoint capabilities. Supervision policy has host and QEMU
fixture coverage; that is not a long-running soak of every hardware service.

The console and terminal have distinct roles:

- `io.console` drains diagnostic log rings to the configured sink and may mirror
  output nonblockingly when the profile provisions it. On Anne, retained native
  logging remains available independently of the interactive terminal.
- `tty.fb0` owns terminal state, text rendering and shell input/output routing.
  It consumes normalized input and serial bytes rather than kernel log rings.
- `io.framebuffer` owns the panel mapping and presentation mechanism. The TTY,
  shell and console do not receive unrestricted display-register authority.
- `msh` uses terminal, filesystem, control and diagnostic endpoints as granted.
  Diagnostics inspect bounded read-only records through the debug-read ABI.
- `dev.apps` supplies a bounded application-stream path. It is not a general
  desktop compositor/window system.

USB class, input and role policy remain separate from controller execution.
QEMU xHCI has isolated DMA buffers; Anne DWC2 uses PIO with CPU payload arenas.
The phone service chain separates I2C, FUSB301 Type-C observation, Hi6523 source
power leases, PHY sequencing, role coordination, host enumeration/HID, gadget
EP0, CDC, bulk, FIDO2 and serial routing. Role changes quiesce the controller and
power before ownership changes. Native platform capabilities permit fixed board
operations, not arbitrary physical register writes. Hardware IRQ bind/ack exists;
it does not mean every driver has stopped polling.

Phone CDC and native shell operation are recorded. Reconnection history includes
whole-device disconnects after the EP0 fixes; do not infer that every host/cable
combination or reconnect issue is resolved. FIDO2 CTAP2 and packed attestation
have implementation and host verification; certificate/browser/relying-party
acceptance must be stated separately.

Details: [USB and serial](usb.md), [FIDO2](fido2.md), and
[Anne operations](../boards/huawei-anne/operations.md).

## AmeFS and durable storage

`io.fs` is a native userspace service. Filesystem-local handles are distinct from
kernel capability handles. Clients use session rings and immutable read arenas;
the kernel does not parse paths, directories, journals or file extents. The
block-service protocol supplies read/write/flush beneath it.

Packaged root objects are read-only; `/tmp` is volatile. With durable backing,
`/data` uses disk-resident keyed B+trees, full-volume allocation bitmaps,
copy-on-write data and a physical metadata redo journal. Ordered data writes,
commit/replay and checkpoint handling preserve crash recovery. Immutable read
arenas retain their backing until clients release mappings/capabilities; cached
pages cannot be reused while a reader still owns them.

QEMU uses `drv.blk.virtio`. Anne uses `drv.blk.emmc`, a 64 KiB uncached ADMA2
arena and the existing roughly 50.6 GiB userdata partition. Its later driver
path uses eight-bit transfers at the verified legacy clock; older PIO-only
bring-up notes are historical. Native read/write/reboot persistence evidence is
recorded in the board storage guide. This is separate from the secure-firmware
RPMB request path, which has unresolved completion/producer limitations.

`FS_SYNC` reports not implemented without durable backing. In-memory CoW
snapshot hooks exist; persistent disk snapshots, tiered storage and system-wide
snapshot recovery are not implemented guarantees. Mount/session semantics remain
bounded and are not a complete POSIX VFS.

[`FS_SPEC.md`](FS_SPEC.md) retains disk/protocol/recovery layouts and working-set
limits. [`storage.md`](../boards/huawei-anne/storage.md) retains eMMC, persistence
and RPMB evidence.

## Dango and Linux appliances

Dango's capability objects cover VMs, memory, vCPUs, channels, shared arenas and
device assignment. The non-VHE monitor saves/restores EL1 state; stage-2 tables
separate guest RAM and device mappings. vCPU exits include WFx blocking, virtual
IRQs, batched MMIO, unsupported traps and host preemption. Native scheduling
controls runner execution; the guest does not schedule host services.

Current fixed bounds include four VMs, four vCPUs per VM, 32 memory objects,
16 channels and 16 shared attachments. Aligned RAM/MMIO uses 2 MiB stage-2
blocks; partial splits check capacity before mutation. Whole-range teardown
must not need a new allocation to revoke device memory.

[`dango_guest_layout.h`](../shared/include/midix/abi/dango_guest_layout.h) is the
shared build-selected memory contract: the default minimal layout is 128 MiB;
the lavapipe layout defines 512 MiB and moves channel/staging/shared/MMIO windows
above RAM. Kernel, manager, DTS and artifact packaging must select a matching
layout. Older notes that call layout selection wholly unimplemented predate this
header. The existence of the larger layout alone does not prove a complete
native lavapipe appliance/session bridge.

The host/guest channel uses shared rings, HVC notifications and virtual IRQs;
Linux exposes `/dev/dango-channel0`. The independent GPU-disabled test exchanges
512 ordered round trips with wraparound. Guest profiles carry only the required
boot/driver/channel runtime. Panfrost starts from an explicit `allnoconfig`
fragment; resolved configuration checks enforce minimality. ARM64's forced SMP
support does not imply that the appliance exposes multiple guest CPUs.

On Anne, an EL1 firmware handoff cannot create EL2. Runtime monitor detection
leaves the native console working when EL2 is unavailable. The tested EL2 route
uses a RAM-patched stock LK plus staged TEE; an ordinary reboot discards that RAM
patch. All-core EL2 entry and installed monitors have recorded phone evidence.
Private firmware, backups and captures remain outside the source tree.

[Appliance builds](linux-appliances.md),
[Anne boot and firmware](../boards/huawei-anne/boot.md), and
[Switch boot](SWITCH_BOOT.md) retain platform workflows and evidence.

## Graphics: native APIs, guest drivers, native presentation

Native applications link Ame's freestanding EGL/GLES or Vulkan clients. The
application process does not link Linux or Mesa. The API-neutral graphics stream
uses libmochi transport; Dango validates and brokers it to the guest renderer.
The Linux/Mesa stack stays inside the appliance.

The verified phone GLES path is:

```text
native EGL/GLES subset → shared command batches → Dango → guest GLES decoder
    → Mesa/Panfrost → Mali-T830 → imported SharedGpu target
    → native framebuffer service → DSS direct scanout → retirement ACK
```

The client implements a deliberately bounded EGL/GLES 2 subset with one
provisioned client/context/surface, VBO-based vertex data and a fixed 512×512
diagnostic surface. `diag gpu` reuses shaders/context/VBO across runs and restores
the console after its five-second animation. This is not a conformant complete
GLES stack or general native window system.

Commands use a CPU-only 32 KiB arena with two bounded slots; the guest copies and
validates batches before decoding. Queries/finish/presentation drain outstanding
work. A transport reply retires command storage; it is not an asynchronous GPU
fence. A separate bounded blob-cache arena lets the manager persist Mesa cache
snapshots through native FS at `/data/mesa-blobs-v1`; guest workers do not write
SPSC channels or receive filesystem authority.

The direct display path imports **two independently owned 1 MiB pixel arenas**.
After GPU completion and DMA-buffer cache synchronization, the native display
mechanism switches the verified inherited DSS pipeline to the completed target.
A fresh VSYNC, live timing and source-address readback establish retirement
before the previous buffer can be reused. Unsupported pipelines retain CPU-copy
presentation. Restoration preserves the console's original plan. On uncertain
retirement or owner death, buffers stay pinned/quarantined; a register write
alone cannot authorize freeing a DMA source. Rendering and presentation remain
serialized; asynchronous GPU fence overlap is future work.

The 22 October direct-scanout checkpoint recorded 298 frames in five seconds and
299 acknowledged transitions including restore. That is throughput/presentation
evidence, not input-to-panel latency. Earlier copy/readback measurements use
different boundaries and cannot be compared directly.

The current experimental native Vulkan frontend is `libVulkan`, with Vulkan 1.0
entry points generated from the pinned Khronos registry. It uses the same
graphics transport and the guest's checked RPC decoder to call PanVK. Pointer
graphs are encoded as bounded offsets, handles become typed remote tokens,
and mapped memory uses explicit bounded upload/download operations. The Ame
diagnostic presentation helper accepts a constrained 512×512 linear image;
standard surface/swapchain WSI is not exposed. `diag gpu vulkan` exercises the
native triangle client. The older reference-session broker and standalone
lavapipe executor have been removed from the current tree; their retained
integration reports are historical.

The phone PanVK fork targets Midgard architecture 5 and has recorded pixel-suite,
triangle and animation results. Those earlier guest-renderer checkpoints do not
by themselves verify every new native frontend path. Transfer variants, layered
rendering, indirect draws, queries and feature reporting require continued
implementation/acceptance. A query patch in the working tree is not evidence of
a CTS pass. No Vulkan conformance claim is made.

QEMU's virtio-gpu/KMS path is another backend. Its foreign DMA-buf import limit
requires a bounded copy; the phone's direct Mali/DSS path must not be generalized
to virtio-gpu. PCI assignment requires a live IOMMU route, fixed BAR validation
and interrupt ownership before bus mastering.

[`graphics.md`](graphics.md) retains API integration, direct-scanout invariants,
PanVK build/coverage and dated optimization results. [`linux-appliances.md`](linux-appliances.md)
retains the separate QEMU/lavapipe integration limits.

## Security and failure boundaries

The implemented security model combines explicit rights, generation-checked
handles, bounded decoding, authenticated images, address-space isolation and
DMA ownership. Kernel paths validate user buffers and untrusted indexes before
access. Structural checks and bounds must remain in hot paths where required;
performance is not permission to omit validation.

Controller exit/reset, IRQ unbinding, VM retirement, shared-memory detach and
display retirement must complete before associated pages can be reclaimed.
Mali teardown resets the device before releasing shadow tables or VM memory;
a failed reset returns busy and retains the live state for retry. Quarantine
prefers retained memory over an uncertain DMA use-after-free.

The tree does not establish a completed microarchitectural isolation policy,
per-principal timer-precision enforcement, hardened hostile-guest GPU isolation,
or end-to-end secure boot across arbitrary firmware. Native RT/WCET verification,
transparent service/VM snapshots, snapshot-and-evict OOM policy, suspend/resume
and hibernation were legacy design goals and remain future work. Filesystem CoW
hooks and supervisor restart are narrower implemented mechanisms.

## Validation and remaining work

Start with [README build/run instructions](../README.md) and `scripts/verify.sh`.
Host probes run production code with privileged boundaries substituted and
ASan/UBSan where appropriate. QEMU fixtures cover real exception, address-space,
SMP, IPC and lifecycle behavior. Opt-in appliance, graphics, IOMMU and panic rows
extend the default gate; inspect the current script for available switches.
Neither host probes nor QEMU replace physical-device acceptance.

Important evidence sources include:

| Concern | Reproducible entry points |
|---|---|
| Capability/object lifetime | `tests/test_capabilities.py`, kernel lifecycle fixture |
| IPC and ring races | `tests/test_ipc.py`, `tests/test_mochi_ring.py`, `tests/test_framed_ring.py` |
| Fair scheduling and frame ownership | `tests/test_scheduler.py`, lifecycle/supervision fixtures |
| Signed policy/packaging | `tests/test_system_descriptor.py`, image/bundle and provenance tests |
| Durable data and recovery | `scripts/test/durable-persistence.py`, store/journal fault-injection tests |
| Linux channel and Mali mediation | `scripts/test/dango-panfrost.py`, `tests/test_panfrost_appliance.py`, `tests/test_channel.py` |
| Native graphics and display lifetime | `tests/test_native_gles.py`, `tests/test_anne_scanout.py`, `tests/test_native_vulkan.py` |
| Phone installation/boot | `tests/test_anne_flash.py`, matching package/readback records and `diag el2` |

Ongoing work includes broader application/window interfaces, graphics API and
Vulkan coverage, appliance reconnect/recovery acceptance, USB reconnect
hardening, persistent snapshots, and physical Switch boot acceptance. Claims
about these must identify which layer was exercised and which package produced
the evidence.

## Documentation map

The architecture is the entry point for system behavior and implementation
boundaries. Keep exact layouts, commands and measurements in these subject
guides rather than adding another status or phase file:

| Guide | Contents |
|---|---|
| [ABI](abi.md) | Programmer conventions, native image integrity, signed descriptor, diagnostic operations |
| [IPC](ipc.md) / [scheduler](scheduler.md) | Transport, fairness, budgets, wake/frame ownership and implementation detail |
| [Filesystem](FS_SPEC.md) | Wire/disk layouts, CoW, journal/replay and bounded working sets |
| [Graphics](graphics.md) | Native clients, direct scanout, PanVK and dated rendering evidence |
| [Linux appliances](linux-appliances.md) | Minimal guest builds, channel tests, QEMU/lavapipe integration and copy limits |
| [IOMMU](iommu-smmuv3.md) | Generic/native/assigned DMA rules and SMMUv3 backend |
| [USB](usb.md) / [FIDO2](fido2.md) | Service boundaries, serial protocol, host tools and attestation limits |
| [Performance](performance.md) | Reproduction, benchmark contracts, baselines and boot/runtime observations |
| [Anne hardware](../boards/huawei-anne/hardware.md) | Board, framebuffer, SMP, power and diagnostic reference |
| [Anne operations](../boards/huawei-anne/operations.md) | Capture, install, serial and application-stream workflows |
| [Anne boot](../boards/huawei-anne/boot.md) | EL2 handoff, bootloader investigations and firmware evidence |
| [Anne storage](../boards/huawei-anne/storage.md) | eMMC/AmeFS hardware results and RPMB investigations |
| [Anne GPU isolation](../boards/huawei-anne/gpu-isolation.md) | External-SMMU routing/firmware research |
| [Switch boot](SWITCH_BOOT.md) | Hekate/ATF/U-Boot packaging and physical investigation |

Third-party licenses/provenance, library-local READMEs and test fixtures remain
beside their owners. Generated output, private firmware and captured logs stay
under their existing ignored artifact directories.
