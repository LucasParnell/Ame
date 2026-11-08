# Native app security audit

Initial pass 2026-10-29; continued 2026-10-30. This native-app audit does not
cover firmware diagnostics or EL3 boot code; the separate ANE-LX1 DTS finding
is documented in [the DTS loader vulnerability report](../boards/huawei-anne/dts-loader-vulnerability.md).
Validation uses QEMU
Cortex-A53 at EL1 with one and four CPUs, plus bounded concurrent host probes
under AddressSanitizer and UndefinedBehaviorSanitizer. No fuzzing or physical
phone operations were performed.

## Findings

### Sandbox principal escalation through delegated spawn authority — fixed

`SYS_SPAWN` checked the supervisor capability and `RIGHT_SPAWN`, but accepted
either native principal class independently of the caller's class. A sandboxed
app given authority to supervise children could request a `native_trusted`
child. In `kernel/sched/thread.cc`, that class has no sandbox page quota and
receives greater scheduler weight. Delegating child supervision therefore
allowed the caller to escape those sandbox restrictions.

The syscall now rejects a trusted child request from a sandboxed caller with
`E_PERM`. Trusted callers still need a supervisor capability with `RIGHT_SPAWN`.
This finding establishes a principal/resource-policy escape, not EL1 code
execution or arbitrary kernel memory access.

The target regression spawns a sandboxed fixture with its own delegated
supervisor and a read-only copy of the executable archive, then attempts to
spawn a valid executable as trusted. It fails against the original syscall
behavior and passes with the new check. A second, trusted fixture receives an
attenuated supervisor without `RIGHT_SPAWN` and verifies that trust alone
cannot authorize spawning.

### Concurrent user-buffer replacement during logging or image loading — fixed

`SYS_DEBUG_PUTS` and `SYS_SPAWN` validated caller mappings and then consumed
their user addresses directly. A sibling thread could unmap or replace those
pages after validation, making the kernel read from a changed mapping or fault
while reading it. Logging now copies bounded chunks through the guarded user
copy path. Spawn snapshots the complete bounded image under the page-table
lock into kernel-owned pages before the loader parses it, then releases the
snapshot on every exit path.

The temporary snapshot is charged to the caller's page quota. It requires a
contiguous allocation and can return `E_NOMEM` under quota or fragmentation
pressure; its maximum remains the syscall's existing 16 MiB image bound.

### Read-only arena authority widened through guest attachment — fixed

`attach_shared_memory()` required only `RIGHT_MAP` on its source arena
capability. Requested guest read/write rights were validated as a bitmask, but
were not compared against that capability. A holder of VM control and an
attenuated read-only arena could therefore map the original host pages writable
in guest stage 2. Attachment now requires `RIGHT_MAP` and every requested data
right from the same retained capability.

The host probe exercises the production attachment entry point with real
capability tables. Its rights regression fails when the old `RIGHT_MAP`-only
check is restored. The target lifecycle fixture also verifies that writable
attachment of a read-only arena returns `E_PERM`, while read-only attachment
succeeds.

### Registered DMA permissions ignored by the IOMMU — fixed

A region registered for device reads could still be installed in an IOMMU
domain with device-write permission: ownership queries carried the memory
class, whose maximum permissions were read/write, but omitted the registered
region's narrower permission set. `PageOwnership` now carries an explicit
permission ceiling within its existing 16-byte layout, and the core checks it
on every page before publishing any translation. Queries that omit the ceiling
fail closed. Native and appliance owners explicitly publish their permitted
access; shared arenas publish their registered permissions.

Registration also rejects permissions beyond the attachment's guest rights.
An existing registration is immutable: changing permissions requires fenced
teardown and a new attachment, rather than returning success while retaining
the old permissions. Tests reject write mappings of read-only registrations
and reject a multi-page mapping whose later page denies write access. Restoring
the old IOMMU checks makes the shared-arena regression fail.

### Late capability grants could repopulate retired process tables — fixed

Process teardown drained a capability table without permanently closing it.
A caller retaining a process handle could grant into the table after cleanup
had passed. A late self-capability could preserve a reference cycle and retain
the dead process, its address space and its PID indefinitely. Destruction
callbacks could likewise reinsert capabilities into slots already swept.

`cap::close()` now publishes a permanent closed bit under the table lock before
draining entries. Allocation, duplication and new lookups reject a closed
table; existing retained borrows remain safe. Both normal process retirement
and final storage destruction use it. Host tests cover reentrant callbacks,
concurrent late grants, idempotent closure and retained-borrow lifetime.

### Shared wired mapping metadata raced unmapping — fixed

`SYS_MAP_PAGE` registered a ring/unmanaged arena's VA claim, mapped its pages,
then attached its lifetime pin as separate operations. A sibling thread could
unmap the range and remove the claim before pin attachment, reaching the
`arena_retain_mapping()` assertion. Unmap also removed PTEs separately from the
metadata update, allowing a newly mapped generation to be confused with the
old one.

AMM now serializes the VA claim, wired PTE publication and backing pin in one
operation. Wired unmap removes the PTE and updates its arena metadata under the
same manager lock. The host probe exercises competing map/unmap operations and
verifies that failed rendezvous retains both mapping metadata and its pin.
Final backing references are detached with the metadata and released after
dropping the manager lock, so a destructor may safely reenter AMM.
Restoring the original split claim/map/pin sequence in the host fixture
reproduces the `arena_retain_mapping()` assertion under competing unmaps.

### Guest IRQ routing accepted inspection-only capability rights — fixed

`device_route_irq()` accepted `RIGHT_READ` on a host IRQ even though installing
the route configures and enables that physical interrupt. It now requires
`RIGHT_WRITE`, matching native IRQ binding and acknowledgement. The host
regression uses production routing and capability tables, verifies that an
inspection-only IRQ causes no route or hardware-install side effect, and
accepts a write-authorized route. Restoring the old read check fails the test.

### Live IPC reply peers could migrate across CPU ownership — fixed

Synchronous handoff stores raw peer pointers and updates the sender's saved
frame. Those operations assume one CPU owns both peers, but work stealing,
wake-affine hints and context rebinding could move a peer while its reply link
remained live. Reply and teardown could then access the linked storage from
different CPUs without the lifetime protection that would require.

Live reply links now prevent stealing, wake migration and context rebinding.
Link creation serializes with both lifecycle guards; link publication is
atomic for remote ownership checks. Reply and abort retain the peer before
unlinking. Reply publishes the sender through the guarded wake path before
clearing its last ownership link; reply-and-wait retains the sole-receiver wake
optimization. Scheduler tests cover all three migration routes, and existing
request/reply and cross-CPU IPC tests pass. These checks establish ownership
preservation; restoring the old work-stealing behavior fails the new ownership
regression. No EL1 exploit was attempted.

## Review coverage and limits

- [x] Review capability duplicate/grant rights-subset checks and ring sharing.
- [x] Exercise both trusted and sandboxed native fixtures through real syscalls.
- [x] Reject rights amplification of a duplicable read-only archive capability.
- [x] Reject writable mappings through that read-only capability.
- [x] Reject substituting a memory capability for process grant authority.
- [x] Reject unmapping a kernel virtual address from either native class.
- [x] Verify fixture teardown restores kernel object and page counts.
- [x] Review bounded ring snapshots and typed message length checks.
- [x] Inject zero, truncated, oversized, and UINT32_MAX wire lengths directly
      into shared ring storage; typed decoding rejects them without changing
      the output value.
- [x] Inject an impossible producer cursor; single and batch reads reject it
      without advancing the consumer or modifying the output snapshot.
- [x] Close the direct user-buffer races in logging and executable loading.
- [x] Preserve source arena attenuation across guest attachment and DMA.
- [x] Check registered DMA permission ceilings on every mapped page.
- [x] Require control authority for guest IRQ routing.
- [x] Permanently close retired capability tables before draining them.
- [x] Serialize shared wired mapping metadata and lifetime pins with unmap.
- [x] Preserve CPU ownership of linked IPC reply peers.
- [x] Run lifecycle regressions with one and four QEMU CPUs and boot the normal
      system bundle on four CPUs.

The source review includes the kernel capability, syscall mapping, shared VM,
DMA and IRQ boundaries above, plus filesystem session-owned handles and
nameservice generation handling. It does not constitute an exhaustive audit of
every service, device backend or possible SMP interleaving. Physical hardware
and hostile guest execution against an assigned physical device were not tested.

The earlier delegated-spawn fix predates the continuation. Other pre-existing
working-tree changes were preserved.

## Continuation validation

The host regressions for capabilities, IPC, scheduler ownership, managed
arenas, shared VM arenas, IOMMU permissions/ordering, device IRQ authority and
typed ring messages passed with ASan/UBSan. Native DMA owner separation,
stage-1 mappings, stage-2 mappings, filesystem handles and IOMMU quarantine
lifecycle checks also passed. The IRQ table probe had stale 256-slot
expectations despite the existing 1024-slot GICv2 implementation; its assertions
now cover INTID 1019 and reject reserved INTIDs 1020..1023.

Both QEMU and Huawei Anne kernel builds passed. No built image was flashed.
Target logs are in `out/qemu-dev/security-audit-up/`,
`security-audit-smp4/`, `security-audit-handoff-smp4/` and
`security-audit-system-smp4/`, with the final helper changes rerun in
`security-audit-final-smp4/` and `security-audit-final-system/`.
Build and rerun the host checks with:

```sh
cmake --build out/qemu-dev --target anko kernel_regressions -j 2
PYTHONPATH=tests python3 -m unittest test_capabilities test_ipc test_scheduler \
  test_amm test_arena test_shared_arena test_iommu test_vm_device_authority test_irq_table \
  test_mochi_ring test_identity_map test_stage2 test_fs \
  test_usb.UsbCore.test_native_dma_owners_have_separate_domains
python3 scripts/test/lifecycle.py
```

## Focused reproduction

Build with `cmake --build out/qemu-dev --target anko kernel_regressions -j 2`.
Run the fixture with:

```sh
python3 scripts/test/boot.py --build out/qemu-dev \
  --bundle out/qemu-dev/lifecycle.mxb --cpus 1 --cpu cortex-a53 --entry-el 1 \
  --timeout 10 --marker 'security authority regression passed' \
  --output out/qemu-dev/security-spawn-after
```

The early marker lets the runner stop before the existing lifecycle stress
matrix. Run only the deterministic host IPC test with:

```sh
PYTHONPATH=tests .venv/bin/python3 -m unittest \
  test_mochi_ring.MochiRing.test_messages_rings_and_protocol_replies
```
