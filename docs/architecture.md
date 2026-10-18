# Midix Architecture

Canonical overview of the system design, invariants, performance contract, and resolved design decisions.

Midix is a microkernel, capability-based operating system designed to stay small in the kernel and push policy, drivers, and compatibility into userspace or virtual machines. The architecture is intentionally ARM-first, with x86 support treated as a later target. It is organised around a small set of hard rules: no ambient authority, no kernel copies on hot paths, snapshot-aware services, strict memory arena enforcement, and a kernel that is fast for code that conforms to its IPC contract and correct (but unoptimised) for code that does not.

The key strategic decision is to use Linux as a driver appliance rather than as the system boundary. Midix remains the authority for scheduling, security, memory mapping, and the user experience, while Linux VMs supply compatibility layers and proprietary driver stacks where needed. This keeps the trusted kernel small without giving up the surrounding ecosystem.

---

## 1. What Midix Is

Midix is not trying to be a general-purpose monolithic OS. It is built around a narrow kernel contract and a set of composable services. The microkernel provides the primitives needed to create secure, deterministic, low-latency software, and the rest of the platform is built from capability-scoped services that can be restarted, snapshotted, and reasoned about independently.

The result is an OS that aims to feel modern on mobile hardware without inheriting the complexity of a large in-kernel driver stack. On systems where vendor drivers are awkward or impossible to integrate natively, Linux runs as a managed appliance inside a VM, with Midix controlling when that appliance runs and what resources it can consume.

---

## 2. Architectural Principles

- Keep the kernel minimal: it enforces invariants, maps and revokes memory, schedules threads, and handles wakeups.
- Move data in shared memory, not through kernel copies, wherever latency matters.
- Treat every access to memory, devices, or services as capability-mediated.
- Make snapshotting a first-class primitive, not a best-effort add-on.
- Apply speculation barriers only at trust boundaries; prefer structural safety over heavy fencing in hot code.
- The kernel is fast for conforming code and correct for everything else. Performance is a contract programs opt into, not a guarantee the kernel tries to provide for arbitrary patterns.

---

## 3. Core Invariants

### Memory Arenas

Snapshot-capable services may allocate writable memory only from kernel-registered arenas. The kernel tracks arena ownership and permissions, and any writable mapping outside the registered set is a fault. This gives the kernel a concrete view of what must be preserved during snapshot or hibernation, and it reduces the surface area for accidental or malicious memory use.

Arena registration is checked in O(1) on the allocation path via per-process arena tables. Snapshot validation is O(A) where A is the number of arenas owned by the service, which is bounded and small in practice.

### Capabilities

All access to devices, shared buffers, and services is mediated by opaque kernel-issued capabilities. Capabilities are revocable and auditable. There is no ambient authority and no assumption that a process can simply reach out and touch global resources. Authority is explicit, transferable only by policy, and removable when the kernel decides a resource should disappear.

### Shared-Memory Data Flow

High-frequency communication is built around pre-mapped shared pages. A sender writes into a ring or buffer, publishes the new state atomically, and the receiver consumes it directly. The kernel only participates when setup, validation, or wakeups are required.

### Trust Boundaries

Speculation barriers are reserved for boundaries that cross privilege or trust domains: kernel-to-user transitions, VM entry and exit. Inside the hot path, Midix relies on masked indexing, fixed layouts, and strict capability rules rather than fencing every branch.

### Linux as a VM Appliance

Linux is not the owner of the machine. It runs in a constrained VM and exists to carry the weight of complex compatibility layers and drivers that are not practical to implement first in native Midix. The host decides when the VM runs, what budget it receives, and what device access it may hold.

---

## 4. Performance Contract

The kernel exposes one IPC mechanism — shared-memory rings with doorbell notification — implemented at the hardware floor. Programs that conform to the ring contract and use the notify primitive correctly experience zero-kernel-cycle IPC *while the receiver keeps up with the producer and the shared mapping is resident*. When the receiver sleeps, the cost is bounded by the Tier 2 doorbell path described in §6: a hundred or two cycles, not zero. A correctness-only fallback exists for non-conforming code; it is not optimised.

The "shared mapping is resident" precondition is explicit. Cold first-touch of a shared ring page incurs a normal page-fault cost paid once at setup; this is not part of the steady-state contract, and the contract does not promise anything about workloads that thrash the ring's TLB entry.

This is the load-bearing performance commitment of the system. The kernel does not try to make arbitrary IPC patterns fast. It exposes a small set of primitives at the hardware floor and documents the rules a program must follow to reach that floor. Programs that follow the rules pay nothing for safety. Programs that do not still work; they pay the cost of the cold path.

The contract is stated in §6 (IPC Model) and made concrete in §19 (Performance Conformance Checklist).

---

## 5. Kernel Architecture

The midix-kernel is data-oriented. Structures are arranged to be cache-friendly, per-CPU data is preferred where possible, and the kernel keeps the shape of hot state predictable: SoA-style descriptors, short critical sections, and a scheduler that can make fast decisions without bouncing through deep object graphs.

Initial kernel responsibilities: capability issuance and revocation, shared-page mapping, the notify and wait-doorbell syscalls, sleep and wake with priority handoff, arena creation and allocation, IRQ routing, timer arming, and Device Tree parsing into the hardware graph.

### No Cross-CPU Locks on the Hot Path

The kernel rule: no hot-path operation takes a lock that another CPU might also be taking. Per-CPU run queues, per-CPU timer wheels, and per-CPU wait lists are the default. Cross-CPU work happens via IPI, and the IPI handler is itself a per-CPU operation on the target side. Anything that would require a cross-CPU lock is restructured or moved to the cold path.

### Cross-CPU Wakeups

When a thread on CPU A needs to wake a thread assigned to CPU B, A does not touch B's run queue directly. Instead, A pushes the target thread onto B's *pending-wakes queue* — a per-CPU MPSC structure with single-atomic push, drained by B alone in its IPI handler — and sends an IPI. The IPI handler on B drains the queue and inserts the threads into B's local run queue. **O(1) push, O(W) drain** for W queued wakes; the drain happens on B's own CPU and touches only B's data structures. There is no cross-CPU lock; the only atomic is the lock-free push, and run-queue manipulation remains a per-CPU operation by construction.

### Scheduling Model

Per-CPU run queues. Wakeup paths can hand execution directly to the right thread when the IPC or IO pattern demands it. Run-queue insertion and selection are **O(log n)** for the priority case and **O(1)** for the FIFO case at a given priority.

Work stealing applies only to the time-shared priority class. Real-time threads, threads pinned via affinity, driver threads with bound LPIs (§11), and host-controlled vCPUs (§16) are exempt from migration. Stealing is criticality-aware: a CPU with no time-shared work running may steal from a peer with backlogged time-shared threads, but it never crosses the RT/non-RT boundary, and the heuristic respects affinity hints set by the IPC layer (e.g., "this thread is the receiver half of a hot ring on CPU 3"). Affinity hints are surfaced through the scheduling cap's `set_affinity_hint` operation and are advisory; the host preserves room for real-time and near-real-time workloads, especially UI and media.

---

## 6. IPC Model

Midix IPC is tiered by cost. The kernel commits to floor performance only at the lowest tier; the higher tiers exist for cases where the floor cannot be reached or is not worth reaching.

### Tier 1: Cross-Domain Hot Path — Shared Rings

Both parties hold capabilities to the same shared pages. The sender publishes data; the receiver consumes it. The kernel is not on the data path.

Steady-state cost when the receiver is keeping up and the shared mapping is resident: **zero kernel cycles**. The "about to sleep" flag in the shared ring page is unset because the receiver is actively draining; the sender's userspace ring library reads the flag, sees it unset, and never issues the syscall at all. The receiver, polling the ring as part of its drain loop, picks up the data without entering the kernel either.

#### Ring Contract

Rings are specified to the byte. Deviation costs throughput; the kernel does not adapt.

- Producer head: 8B value, padded to its own cache line.
- Consumer tail: 8B value, padded to its own cache line.
- Slots: cache-line aligned, never spanning lines.
- Cache line size: 64B baseline. Targets with 128B lines pad head, tail, and **all control-ring slots** to 128B. Data rings keep 64B descriptor slots even on 128B targets, because data-ring descriptors are not producer-shared (the descriptor refers to a payload arena owned by the producer; only the descriptor index is read by the consumer, and adjacent descriptors are written by the same producer).
- Control-ring slot size: `max(64B, line_size)` of the target. This is the rule that prevents false sharing between adjacent control slots when the ring is MPSC.

For control rings, slot size is line-sized fixed. For data rings, slots are 64B descriptors referring to a payload arena, so the slot itself stays in one cache line and the payload is paged in by the consumer.

#### Memory Ordering

Minimum ordering required for correctness on weak-memory hardware:

- Producer fills slot with relaxed stores, then writes head with `STLR` (release).
- Consumer reads head with `LDAR` (acquire), then reads slot with relaxed loads.
- No `DMB` unless the ring crosses into device memory.

One acquire and one release per message. Programs that emit `DMB SY` defensively pay 5–20× per operation; this is the programmer's choice and the kernel does not work around it.

A header-only library (Rust + C) implements the contract correctly. The library is the spec for application authors; the text spec exists for compiler and runtime authors who need to inline the operations.

#### Multi-Producer / Multi-Consumer

SPSC is the fast case. MPSC and SPMC pay one atomic per operation. MPMC pays more. These are separate contracts; the kernel does not pretend they are the same.

### Tier 2: Cross-Domain Warm Path — Doorbell Notification

When the receiver is asleep, the sender calls `notify(cap)`. The kernel's work is bounded:

1. Validate the capability handle. **O(1)**: one indexed load, one generation compare.
2. Check receiver state. One load — is the receiver sleeping on this doorbell?
3. If the receiver is sleeping on this doorbell: if same-CPU, mark runnable on the local run queue and return; if cross-CPU, push onto the target CPU's pending-wakes queue (§5) and send an IPI, then return. **O(1)** in either case. If the receiver is awake: bump the doorbell counter and return.

Target budget: under 200 cycles for the cross-core wake case, under 100 cycles for the already-awake case.

The receiver-side primitive is `wait_doorbell(cap, expected_value)`. Semantics: if the doorbell counter is still `expected_value`, sleep; otherwise return immediately. This is futex-shaped.

The "I'm about to sleep" flag lives in the shared ring page itself — written by the receiver, read by senders directly from userspace. No syscall is involved in the check. Before sleeping, the receiver writes the flag and then calls `wait_doorbell`. The sender's userspace ring library reads the flag with a single load before deciding whether to issue `notify`. If the flag is unset, the sender publishes data and continues without entering the kernel at all. If the flag is set, the sender issues `notify(cap)`, which performs the wake work above.

This is the mechanism that delivers genuinely zero-kernel-cycle IPC in the steady state. The kernel is entered only when sleep is required *and* the receiver is in fact asleep. The race between flag-set and counter-bump is handled by the standard futex pattern: a notify that arrives at an already-awake receiver is harmless, because the receiver compares the counter against its expected value and skips the sleep.

### Tier 3: Synchronous Hand-Off — Opt-In

For latency-critical pairs, `notify_handoff(cap)` performs a synchronous switch on same-core sender/receiver where the receiver is sleeping on the right doorbell. The sender's timeslice transfers to the receiver. seL4 calls this `Call`-style semantics; it is safe because the kernel enforces both the cap check and the scheduling decision.

Combined with a register-passing payload convention (the first N argument registers of the syscall are made available to the receiver in matching registers on resume), this closes most of the gap with L4-family register IPC for small synchronous request/reply across trust domains. **Target budget: under 400 cycles cross-domain same-core for handoff with register payload, measured on the bring-up target.** Opt-in because the sender blocks until the receiver yields back, which is correct for RPC-shaped patterns and wrong for fire-and-forget.

#### Priority Donation on Handoff

`notify_handoff` uses priority donation, not priority transfer. The receiver runs at the **maximum** of its own priority and the sender's effective priority until either (a) the receiver issues the matching reply via `reply_handoff`, or (b) the receiver's quantum expires under the donated priority. On reply, the donated priority is released and both threads return to their natural priorities. This rules out the inversion where a high-priority sender hands off to a low-priority receiver and then waits behind unrelated medium-priority work. Donation is non-transitive in the v1 contract: if the receiver itself issues a further `notify_handoff`, the donation does not chain. Chained donation can be added later if measurement justifies it; the v1 stance is that chains indicate a structural problem and should be flagged by the diagnostic harness.

### Tier 4: Cold Path

Setup, teardown, capability transfer, backpressure handling. A slower kernel path is acceptable. Not optimised.

### Power Behaviour

Adaptive hybrid polling falls out of Tier 2's flag-check protocol. Real-time services spin briefly to catch nearby work, then call `wait_doorbell` when the latency budget no longer justifies CPU burn. The kernel sees only the cases where sleep is required.

---

## 7. Capability System

### Representation

Per-process handle table, dense and integer-indexed. Handle format: 32-bit index + 32-bit generation, packed in a 64-bit register.

Cap table entries are exactly one cache line:

```
struct cap_entry {
    void*    target;         // page, IPC port, service, etc.
    u32      rights_mask;
    u32      generation;
    u16      type_tag;
    u16      flags;
    u8       padding[44];
};
```

Total: 64B. Entries are sized to the smaller of the two supported cache line sizes (64B), which guarantees single-line fetch on 64B-line targets. On 128B-line targets the entry occupies half a line; this is intentional. The §6 rule of padding to the *larger* line size addresses producer/consumer false sharing on ring indices, and that concern does not apply here: a cap entry is read by the single owning process and written only by the kernel during issuance or revocation. There is no hot-path sharing to defend against. The cost of using 128B entries unconditionally would double the cap-table footprint for no correctness or hot-path benefit.

### Lookup

```
entry = cap_table[handle.index]
if entry.generation != handle.generation: fault
if entry.type_tag != expected_type: fault
return entry.target
```

A single cache-line fetch followed by two compares against register-resident handle fields. No pointer chasing. **O(1)** lookup, branch-predictable in the steady case.

Processes with up to 1024 caps use a flat table (64 KB per process, well-cached). Above that threshold, a two-level table pays one extra load per lookup. Still O(1). The 1024 figure is a starting point chosen to keep the flat table within an L1-friendly footprint for typical service workloads; the threshold is tuned with telemetry once real workloads exist.

### Revocation

Revoke marks the cap object's generation as dead. Handle lookups compare generation; mismatches return invalid-cap. **O(1)** revocation regardless of how many handles point at the cap. The handle table is never walked at revoke time.

### Target Lifetime

Cap entries hold a reference on the target object. Multiple caps may reference the same target — a shared page mapped into several processes is the canonical case — so each target carries a refcount of extant caps. Revoking a cap drops the entry's reference; when the refcount reaches zero, the target is freed. Refcount manipulation happens on issuance, transfer, and revoke — all cold paths — and never touches the lookup hot path.

`revoke_subtree` walks the derivation tree, drops the reference held by each cap in the subtree, and frees any target whose refcount reaches zero inline with the walk. **O(D)** for the walk plus O(F) for the frees, where D is the subtree size and F ≤ D. Both costs are cold-path. The lookup hot path remains a single indexed load and a generation compare regardless of how complicated the underlying lifetime graph becomes.

### Derivation Tree

Lives off to the side, used only for `revoke_subtree` operations and audit. Not on the lookup hot path. Subtree revocation is O(D) where D is the size of the derived subtree, which is acceptable because revocation is a cold-path operation.

This is roughly Zircon-shape with seL4-shape revocation semantics: handle-table speed, capability-object correctness.

### Principals

Every capability is held by some *principal*. A principal is the kernel-tracked identity to which capability ownership and per-cap policy attach. Principals are kernel objects, not user-defined strings; the kernel issues a principal token at process spawn and that token is the identity for all subsequent policy decisions (timer precision, scheduling class, arena quota, IRQ binding eligibility).

Principal classes in v1:

- **kernel**: trusted, full hardware precision, no policy capping.
- **native-trusted**: native services in the boot manifest. Full precision unless the manifest assigns otherwise.
- **native-sandboxed**: native processes spawned with a restricted nameservice cap and reduced-precision time policy. The browser content process is the canonical example.
- **vm**: a VM as a whole. Precision and scheduling policy are set per-VM, not per guest thread (the kernel cannot see guest threads).

Principals do not nest. A native service spawning a sandboxed child creates a new principal of class native-sandboxed; the child's caps are issued against that new principal, and the parent's principal is not implicated in the child's policy decisions.

User-level identity (the human user, multi-user accounts) is **not** a kernel concept. It is a userspace identity service that maps human users to principal tokens at session start and is itself a normal capability-mediated service. The kernel sees only principals.

---

## 8. Memory Arenas and Quotas

### Arena Classes

**Fixed-size arenas.** Declared at creation, never grow. Allocation failure returns an error to the service, which decides how to react. Used for kernel-critical allocators and anything where unbounded growth is a bug.

**Growable arenas.** Have a soft cap and a hard cap, both capability-mediated. Growth past the soft cap is allowed but signals back-pressure to the service via cold-path IPC. Growth past the hard cap fails.

The kernel never silently grows an arena. Growth is an explicit operation requiring an arena-grow capability, which is held by a quota manager. The quota manager is policy; the kernel is mechanism.

### Wired Arenas (DMA)

Wired arenas are pinned, have a stable IOVA range, and are registered with the SMMU at the right stream ID at allocation time. The cap to a wired arena can be derived into a DMA cap carrying `(stream_id, IOVA_base, length)`. The driver thread holds the CPU-mapped cap; the device service holds the DMA cap. Same memory, two views, both kernel-tracked.

A buffer service in userspace owns the policy: who can request wired memory, size limits, fragmentation. The kernel only enforces structural rules: writable + wired requires explicit arena creation, requires SMMU registration, registration uses the caller's authorised stream IDs.

### OOM Policy

System-level OOM is the quota manager's problem, not the kernel's. The quota manager can:

- Refuse growth.
- Kill a service (revoke its caps; the supervisor handles restart).
- Snapshot a cold service, drop it, restore later when memory pressure eases.

The "snapshot-and-evict cold services" mechanism requires a configured flush target — eMMC, NVMe, or a swap arena explicitly enrolled at boot. The quota manager invokes snapshot with `flush=true` for eviction; the snapshot blob is written to the flush target and the in-memory arena pages are released. **Without a configured flush target, snapshot-and-evict is unavailable to the policy and the quota manager must fall back to refuse-growth or kill.** This is stated explicitly so that bring-up configurations without working storage do not silently lose the OOM option.

---

## 9. Snapshotting and Service State

Snapshotting is a core platform feature. Midix-native services and Linux VM snapshots are explicitly separate mechanisms and must not be conflated.

### Native Services

Services explicitly register the memory regions and queues that define their state via arena registration. The kernel validates that the writable address space is covered by registered arenas before allowing a snapshot to proceed. Validation that the writable address space is covered by registered arenas is **O(A)** in the number of arenas, bounded per service. The capture itself is **O(M)** in the total registered memory, since contents must be written to the snapshot blob; for large services this is the dominant cost and is the cost the system actually charges for taking a snapshot. The two should not be conflated.

A snapshot includes:

- Thread register sets.
- Address-space mappings.
- Registered arena contents.
- Serialisable IPC queues.
- The capability set needed to reconstruct the service.

### Linux VM Snapshots

The Linux VM is snapshotted as an opaque blob: vCPU registers, stage-2 mappings, virtio ring state. No CRIU, no guest cooperation required for correctness. The paravirtual agent can optionally signal a quiesce before snapshot; this is an optimisation, not a requirement.

### Recovery as the Failure Path

Snapshot-and-restore is the recommended recovery path for stateful services. The contract: snapshot frequency is the service's choice, recovery is supervisor-driven, and lost work is bounded by snapshot interval. Service authors design around this explicitly.

---

## 10. IO Services

Midix uses small IO services, each owning a narrow domain. The first two are `io.framebuffer` and `io.console`.

`io.console` is the simplest practical user-visible service and the right first service to build. Deterministic, capable of writing to UART when framebuffer access is unavailable, and able to maintain a software text buffer so that logs and early boot output remain visible even if graphics are not yet ready.

`io.framebuffer` receives the boot framebuffer capability and can grant mapped memory or DMA-BUF style resources to other services. If a VM takes exclusive ownership of the display hardware, the kernel revokes the capability from the host-facing service and hands the device to the VM according to policy.

---

## 11. IRQ Delivery

GICv3 LPIs are the primary mechanism. Each driver service holds a notification capability bound to one or more LPIs. The kernel ISR is trivial:

1. Acknowledge the GIC (EOI write to the appropriate system register).
2. Validate the bound doorbell cap (one indexed load, one generation compare — one cache line).
3. Wake op: insert into the local run queue, or push onto the target CPU's pending-wakes queue (§5) and send an IPI.
4. Return.

**Concrete cost:** GIC EOI + one cache line for cap validation + one cache line for the wake op (local run queue insert or pending-wake push), plus an IPI register write on cross-CPU. Target budget: under 150 cycles for local delivery, under 250 cycles for cross-CPU. The driver thread wakes, reads device state, processes, re-arms — that part is a regular high-priority thread waking from a notify and is not counted in the kernel ISR budget.

LPI redistributor target follows the driver thread's CPU affinity. The scheduler updates the redistributor target when it migrates the thread, so interrupts land near the consumer. For latency-critical drivers (touchscreen, audio), pin both the thread and the LPI.

No userspace ISR. No interrupt threads with elevated priority that are not normal scheduler citizens. For shared interrupts (legacy PCIe, mostly irrelevant on ARM), a kernel-side demux service fans one LPI out to multiple driver notifies. The cost is paid only when needed.

---

## 12. Time and Clocks

### Monotonic Clock

`CNTPCT_EL0` mapped readable to userspace for principals of class `kernel` and `native-trusted`. No syscall. Native trusted services get full hardware precision.

### Wall Clock

A small `time` service owns the RTC and exposes a query/subscribe interface. Updates land as notifies on subscribed clients. Wall and monotonic are never conflated anywhere in the system.

### Timers

The kernel offers a timer capability with relative-deadline `arm(delta)` semantics. Expiry posts a notify. Per-CPU timer wheels inside the kernel; capability holders do not need to know about hardware timers. **O(1)** arm and fire on the bucketed wheel; **O(log n)** for far-future timers held in a heap.

### Untrusted Code Precision

Sandboxed and VM principals do not get direct `CNTPCT_EL0` access. The mechanism is per-principal-class:

- **native-sandboxed:** `CNTPCT_EL0` reads from EL0 are undef-trapped by clearing `CNTKCTL_EL1.EL0PCTEN` for the process. The libc redirects high-frequency timer reads to a **mapped quantized-counter page**, written by the time service at the policy granularity (e.g., every 100 µs for the browser content class). Userspace reads the quantized value with a single load, no syscall, lower precision. The undef trap is the slow correctness path for code that bypasses libc and reads the register directly; it costs an EL1 trip per read and is the price of bypassing the contract.
- **vm:** the VM's view of the architectural counter is virtualized via `CNTVOFF_EL2`. Hard precision capping for the VM (e.g., capping the guest's view to 1 µs steps when the hardware is nanosecond) requires `HCR_EL2.TID2`-style trap-and-emulate of counter reads, which is **expensive** (every read becomes a VM exit). For VMs, the v1 contract is that precision capping below the hardware resolution is opt-in per-VM and is understood to slow every counter read in the guest. Most VMs run without the cap and rely on the per-cap policy on time-related caps issued *by* the host instead.
- **time service mediation:** for principals that need monotonic time but not high frequency, `time_query(cap)` returns a rounded value via Tier 1 ring. No trap, no syscall.

The "reduce timer precision for untrusted code" rule is concrete: per-cap configuration on time-related caps, plus the per-principal-class register-access policy above.

---

## 13. Service Discovery and Bootstrap

Init holds a root capability to a `nameservice` userspace process. Nameservice maps strings → caps. Anyone with a nameservice cap can ask, but the answer is policy-mediated: nameservice can refuse, return a wrapped or proxied cap, or hand over the real one.

Nameservice is just a service — restartable, snapshot-capable, no special kernel status.

Sandboxing falls out for free: a process given a restricted nameservice cap (one that resolves only a subset of names) is structurally limited in what services it can reach.

The boot-time service set is established by init walking a manifest, spawning each service, and registering it with nameservice. The manifest is data, not code, so it is easy to swap for different system configurations (phone, dev box, headless).

### Restart Behaviour

The supervised recovery path for nameservice is **snapshot-and-restore**. The cap to nameservice points at the service object, not at a particular incarnation, so a snapshot-restore cycle does **not** bump the service's generation and clients do not see invalid-cap errors. Outstanding queries either complete after the brief restore window or return a transient `service_unavailable` error code distinct from `invalid_cap`. Clients have policy: retry-with-backoff is the default in the nameservice client library.

Cold restart of nameservice (snapshot lost, manifest re-walked) is a separate rare path that **does** bump the generation. It is a recovery-of-last-resort and clients must be prepared to re-resolve their nameservice handle through their parent supervisor. The boot-time path through init is the same path used for cold restart.

---

## 14. Failure Model and Supervision

Every service has a supervisor cap held by its parent (init, or another service). Service death — crash, OOM kill, voluntary exit — fires a notify on the supervisor. The supervisor decides: restart from latest snapshot, restart cold, escalate to its own supervisor, leave dead.

### Capability Fate on Death

All caps issued *to* the service become invalid (generation bump, as in §7). All caps issued *by* the service to its clients become invalid the same way. Clients see their handles fail on next use, which is the right time. No proactive notification is needed unless the client subscribed to a liveness signal.

### Dependency Loss

Services can subscribe to a `dependency_lost` notify keyed on caps they hold. When the FS service dies, services that hold FS caps receive the notify and choose their response (degrade, queue, crash). This is a normal IPC pattern, not a kernel feature.

---

## 15. GPU Strategy

### Mediated Access Model

The default GPU strategy is mediated access. The VM produces command buffers; a Midix-side GPU service handles them via resource-level validation rather than command-stream interpretation. Concretely: the GPU service walks each submission to extract resource handle references — buffers, textures, descriptor sets, indirect buffers, fence handles — and validates each handle against the consumer's cap set, and validates that all referenced address ranges fall within the consumer's registered arenas. **Command opcodes and pipeline state are not interpreted.** The walk is structural (pointer-chasing the submission to find references), not semantic (understanding what the commands do).

Two enforcement layers, distinct and complementary:

- **Memory boundaries: SMMU.** Hardware enforces that the GPU cannot touch memory outside the VM's arenas. This is the actual containment story for memory faults.
- **Capability scoping: GPU service.** Software validates that the VM is not naming resources it does not own. This is not equivalent to IOMMU enforcement and should not be described as such.

### Liveness and Recovery

Resource-handle validation contains memory faults. It does not contain GPU liveness. A bad command stream can hang the GPU or trigger a context-wide reset that affects every consumer.

Each VM (and each native consumer) holds one or more GPU contexts owned by the GPU service. The hardware reset granularity becomes the contract:

- **Per-context-reset hardware** (modern Adreno, recent Mali): a bad submission kills the context. The consumer receives a `device_lost` notify, and re-creation is a normal cap-mediated operation. Maps cleanly to Vulkan's `VK_ERROR_DEVICE_LOST`.
- **No reliable per-context reset**: a global GPU reset is a residual liveness risk affecting all consumers. The mediated model contains memory faults, not GPU liveness. This is stated explicitly.

The GPU service runs a watchdog on submission queues. A context whose fences do not retire within a deadline is marked degraded; further submissions return error until the consumer acknowledges and resets. Combined with snapshotting, the recovery story is: snapshot the VM, kill the GPU service, restart, resume the VM, re-create contexts.

### Passthrough

Passthrough remains an optimisation, not a dependency. Used only when hardware has strong IOMMU/SMMU isolation and a reliable device reset path. Vulkan support begins in the Linux VM through Mesa, with native Midix userspace rendering left as a later optimisation.

---

## 16. Virtual Machine Subsystem

The VM subsystem hosts compatibility environments; it is not the centre of the OS. The intended appliance is a minimal Linux build with Mesa enabled so that desktop-style applications and graphics stacks can run inside the VM.

### Scheduling and Real-Time Threads

The host controls VM entry through a scheduling interface carrying reason, budget, deadline, and core-mask information. For real-time paths the host does not trust the Linux guest scheduler. The paravirtual agent pins guest RT threads to specific vCPUs, and the host controls those vCPU timeslices directly.

The guarantee is at the **vCPU level**, not the guest thread level. The paravirtual agent is responsible for placing RT threads on RT vCPUs; this is a cooperative contract between host and agent, not one the host can enforce. Optional verification is available via PMU sampling or a `current_thread` hypercall.

#### Verification Cost

Verification is **opt-in, sampling-based, and costs measurable runtime**:

- The `current_thread` hypercall is a VM exit by definition. Continuous use is incompatible with the "avoid VM exits on the hot path" rule and is therefore not the contract; it is a debugging and audit tool, used at a bounded sampling rate.
- PMU sampling avoids VM exits but contends with any guest use of the PMU (perf-style profiling). Workloads that rely on guest PMU access must accept that verification samples will appear in their counters, or disable verification for those VMs.

The v1 stance is that verification runs at a low sampling rate for audit, not as an enforcement mechanism. Enforcement is structural: the paravirt agent pins, the host schedules vCPUs. Verification confirms the agent is doing what it claims.

For workloads that need hard real-time (audio, sensor fusion, anything where a missed deadline corrupts state), the answer is a native Midix service, not a vCPU. The VM-RT story is for soft RT — UI, media playback, input — where good enough most of the time is acceptable.

### Communication

Most communication happens through shared rings and doorbells to avoid VM exits on the hot path. Exits are reserved for exceptional or batched operations.

### Linux as Driver Appliance — Non-GPU Devices

The "Linux as VM appliance" strategy generalises beyond GPU. For any device whose only mature driver is Linux — common cases are Wi-Fi chips with vendor binary blobs, certain camera ISPs, and specialised sensors — the model is:

1. The device is **SMMU-isolated** and assigned to a dedicated *driver VM* (a minimal Linux instance configured to run only the driver and a paravirt agent).
2. The driver VM does not see other Midix services or the rest of the system. It sees the device, its IOMMU window, and a single virtio-shaped paravirt channel back to the host.
3. The host exposes the device's logical interface as a normal Midix service. For Wi-Fi: a `net.wifi` service, holding a cap to the driver VM's paravirt channel, and exposing the standard network service ABI to the rest of the system. For a camera: a `media.camera` service, similarly.
4. Hot-path data (network packets, camera frames) flows over shared rings between the driver VM's paravirt agent and the host service. **Control plane** operations (associate, disassociate, frequency change) cost VM exits and are paid on the cold path.
5. The driver VM is supervised like any other service. Crash → snapshot-restore → device session re-established. Clients of the host-side service see `dependency_lost` notifies during the restore window.

This has the same shape as the GPU mediated model: SMMU containment for memory, capability-mediated boundary for the logical interface, snapshotting for recovery. The cost is one Linux VM per such device, which is not free in memory but is acceptable on the bring-up class hardware. Devices for which a native driver is feasible (UART, simple I2C, GICv3 itself) are not handled this way — they get a native driver service per §11.

---

## 17. Filesystem

### Structure

B+tree for metadata, extents-based layout for data. Tree nodes are stored in registered arenas, making the FS service snapshot-capable like any other Midix service. The kernel has no visibility into FS internals and treats the FS service as an ordinary capability-mediated process.

B+tree operations are **O(log n)** for lookup, insert, and delete. Extent allocation is amortised O(1) for sequential workloads.

### Zero-Copy

Large payloads (VM images, GPU assets) are handled by issuing capabilities to the underlying physical pages directly rather than copying through a VFS layer. The FS service grants a mapped capability and the consumer reads straight from it. No intermediate buffer.

### Copy-on-Write Semantics

CoW policy is a property of the **inode**, not the capability. Files are tagged at creation with an intrinsic class:

- **content-store** — game assets, browser cache, GPU shader blobs, VM images. Written once, read many. CoW-off by default.
- **user-data** — saves, config, databases, documents. CoW-on by default; preserves crash consistency and rollback.

The capability used to open the file selects the *view*, not the policy. The conflict case (both classes referencing the same file) cannot arise because the inode tag is single-valued.

**Hardlinks share an inode and therefore share the policy class.** Two paths cannot disagree on CoW class; the policy is a property of the inode. Per-extent overrides remain per-extent and are visible through any path to the inode. If a workflow needs two paths with different policy classes, the answer is `promote` or a copy, not multiple hardlinks.

The legitimate dual-use case — a downloaded VM image later mutated by the guest — is handled by an explicit `promote` operation. `promote(content_store_cap)` returns a new user-data inode with CoW-on, sharing extents until first write. This is a fork at the FS layer rather than the page layer: auditable, explicit, no ambiguity.

Per-region overrides on top of the inode default still apply: a save-game file inside a content-store-classed package can be marked CoW-on at the extent level. The principle is that the file's metadata records the policy; capabilities reference the policy and do not define it.

Non-CoW extents can be mapped as read-only shared capabilities to multiple consumers simultaneously with zero page duplication, which matters for game textures served to both the GPU service and the compositor.

### Tiering

Hot metadata on fast storage. Cold data (VM images, archived snapshots) on slower tiers. Explicit prefetch hints in the API let callers declare access intent ahead of time, which matters for game loading latency and VM resume time.

---

## 18. Security and Micro-Architectural Policy

Arena enforcement narrows the places where writable state can exist. The platform prefers IOMMU-first isolation for devices. If a device cannot be safely isolated, the system uses a mediated model rather than pretending direct exposure is free.

Fences are used where trust changes, not everywhere:

- Kernel-to-user transitions.
- VM entry and exit.
- Syscall return paths that handle untrusted indices.

BHB flushing, predictor partitioning, and KPTI-style page table separation are applied at VM boundaries where the threat model justifies them.

### Masking-at-Entry Rule

Inside the kernel hot path the rule is structural, not discretionary: **every index that originates from untrusted code is masked to a power-of-two range at the kernel entry boundary, before being used in any speculative path.** Cap-table lookup, ring-slot indexing, and syscall-argument parsing all rely on this rule to make speculative out-of-bounds reads structurally impossible. The kernel can omit per-branch fences in hot code only because masking-at-entry is applied consistently; relaxing it for any code path would require reintroducing fences along that path, and that trade is not worth taking. This is the rule that §4's "structural safety over fencing" depends on; it is not an optional optimisation.

### Timer Precision

Timer precision for untrusted principals is reduced as described in §12. Concrete mechanism: per-cap granularity policy on time-related caps, plus the per-principal-class register-access policy.

---

## 19. Performance Conformance Checklist

Programs that follow the rules below experience floor performance. Programs that do not still work; they pay the cost of the cold path. The kernel does not optimise for partial conformance.

1. Use the provided ring library, or implement the byte-exact contract from §6.
2. SPSC where possible. Pay the atomic for MPSC only when needed.
3. `STLR` / `LDAR` for index updates; relaxed for slot contents. No defensive `DMB SY`.
4. Batch. Drain the ring fully before checking the doorbell.
5. Spin briefly before calling `wait_doorbell` if latency matters more than power.
6. Co-locate hot endpoints on the same NUMA node when relevant.
7. Producer and consumer indices on separate cache lines. Nothing else on the index lines.
8. Capability handles are integers indexing a flat table. Treat lookup as one cache line of work; do not wrap caps in indirection.
9. For RPC-shaped patterns across trust domains where the sender and receiver can be co-located on the same CPU, prefer `notify_handoff` over `notify` + `wait_doorbell`. Cross-CPU RPC has no equivalent fast path: pay the wake-and-wait cost, or restructure to keep the pair co-resident via affinity hints.
10. Use registered arenas for any memory the service expects to survive a snapshot.
11. Keep shared ring pages resident. Workloads that thrash ring TLB entries are outside the steady-state contract.

A program that follows all of these gets the floor. A program that follows none gets correctness via the cold path. There is no middle tier the kernel is responsible for optimising.

---

## 20. System Suspend and Resume

System suspend is built on the same machinery as service snapshotting. There is no separate suspend path.

### Suspend

On suspend request:

1. The host enters a quiescent state: all non-RT time-shared work is preempted; RT services receive a `pre_suspend` notify and finish their current quantum.
2. Each subscribed service snapshots its registered arenas to its configured snapshot target. Services that do not subscribe to suspend are killed, not snapshotted; their supervisor restores them on resume from the latest available snapshot.
3. Driver services place their devices in the lowest power state they can re-enter without re-initialising firmware. Devices for which this is not feasible (some Wi-Fi chips, some GPUs) are torn down fully and re-bound on resume; their host-side service handles the gap as a `dependency_lost` event.
4. VMs are snapshotted via the §9 opaque-blob mechanism. The paravirt agent's optional quiesce is used here if available.
5. The kernel's own state — per-CPU run queues, timer wheels, capability tables — is small and is written to the suspend blob directly. The blob is flushed to storage.
6. The platform enters the deepest sleep state the wakeup-source set allows.

### Wakeup Sources

Wakeup is mediated by the IRQ subsystem. Capabilities to LPI bindings carry a `wakeup` flag. On suspend, the GIC is configured to wake the platform only on LPIs whose bound caps have the wakeup flag set. The set is small by default (power button, RTC alarm, modem ring, lid switch on devices that have one) and is policy-controlled at boot manifest time. Services do not get to elevate their LPIs to wakeup without an explicit cap from the supervisor.

### Resume

Resume is the reverse:

1. Kernel state is restored from the suspend blob.
2. RT and core services are restored from snapshot first. The console comes up early so failures during resume are visible.
3. Driver services rebind their devices. Devices that lost state are re-initialised; their host-side service emits `device_reset` notifies to clients.
4. Time service updates the wall clock from RTC and notifies subscribers of the discontinuity. Monotonic time does not jump (it is `CNTPCT_EL0`-derived); wall clock does, and clients are expected to handle it.
5. VMs are resumed last. Their paravirt agents are notified that suspend occurred so guest software can adjust (NTP step, network reassociate, etc.).

### Failure During Resume

If a service fails to resume from snapshot, the supervisor falls back to cold restart for that service. Cold restart of a service during resume is treated as a normal failure and does not abort the system resume; the system comes up with that service in its initial state and clients see `dependency_lost` if they had outstanding caps to it.

---

## 21. Development and Bring-Up Plan

Toolchain: CMake, ccache, distcc, containerised builds. Early work in QEMU exercises the scheduler, IPC, and memory model before real hardware. First real hardware target: PinePhone-class. After that, a device useful for GPU experiments (Exynos-based phone or similar), then x86.

The PinePhone-class bring-up target uses Mali-400, which lacks reliable per-context reset. GPU bring-up therefore lands in the global-reset residual-risk regime described in §15, not the clean `device_lost` recovery regime. The per-context recovery path is implemented and tested against the second hardware target. The GPU service is structured to support both regimes from the start, but bring-up demos must not assume per-context recovery is available, and the watchdog policy on Mali-400 is "log and surface; do not pretend to recover."

### Boot Flow

The boot sequence is fixed at v1:

1. Bootloader (U-Boot or equivalent) loads the kernel, the boot manifest, and the initial service blobs into memory at known addresses.
2. The kernel initialises per-CPU state, parses the Device Tree into the hardware graph, sets up the cap table for the init principal, and creates the boot arenas.
3. The kernel spawns **init** as the first userspace process, handing it the root nameservice cap, the manifest cap, and the supervisor cap for itself.
4. Init walks the boot manifest. For each entry, init spawns the named service, creates its principal, hands it its declared caps, and registers it with nameservice.
5. Once the manifest is fully walked, init transitions to its steady-state role: parent supervisor for the boot-time service set, and the source of cold-restart capability for nameservice itself.
6. The console service prints the boot-complete marker. The system is up.

The manifest format is data, not code, so different system configurations (phone, dev box, headless) swap the manifest without kernel changes.

---

## 22. Minimal First-Stage Deliverables

- Process manager, arena allocator, shared-page mapping, notify and wait-doorbell syscalls, scheduler, UART driver, Device Tree to hardware graph translation.
- Capability table implementation per §7, including the principal token mechanism.
- `io.console` as a userspace service.
- IPC ring contract written down at byte level, with reference Rust + C library.
- Snapshot primitives able to validate a simple service such as the console.
- First VM prototype running a tiny Linux environment printing over virtio-console, with host controlling vCPU entry and shared-ring communication.
- Performance harness that measures hot-path notify cost and verifies the zero-kernel-cycle steady state.
- Boot manifest walker in init, sufficient to bring up console + nameservice + one demo service.

---

## 23. Canonical Companion Documents

The following documents are derived from this overview and hold the implementation-level detail:

- `IPC_SPEC.md` — exact ring layouts, syscall semantics, memory-ordering rules, handoff protocol, priority-donation rules.
- `IO_SERVICES.md` — startup sequences, console and framebuffer service ABIs.
- `VM_SPEC.md` — VM control ABI, scheduling interface, paravirtual agent contract, driver-VM channel format.
- `SNAPSHOT_SPEC.md` — snapshot formats for native services and Linux VMs, suspend blob format.
- `FS_SPEC.md` — B+tree layout, extent format, CoW promotion semantics, prefetch API.
- `GPU_SERVICE_SPEC.md` — capability validation rules, context lifecycle, watchdog and recovery contract.
- `CAP_SPEC.md` — handle format, table layout, derivation tree, revocation semantics, principal model.
- `SCHED_SPEC.md` — run queue structure, priority model, vCPU scheduling for VMs, affinity-hint interface.
- `BOOT_SPEC.md` — manifest format, init responsibilities, cold-restart semantics for nameservice.

---

Midix is defined by invariants, not by feature count. Any proposed feature that weakens the rules above, forces the kernel to absorb policy that belongs in userspace, or compromises the floor performance contract should be treated as suspect until proven otherwise.
