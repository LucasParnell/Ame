#pragma once
// Bring-up diagnostics. EL3 metadata queries invoke firmware; explicit EL3
// research operations can also modify shared memory. CPU capability and
// ownership of the EL2 monitor are separate.
#include "midix/abi/types.h"

constexpr mx_u32_t MX_DIAG_VERSION = 1;
constexpr mx_u64_t MX_DIAG_READ_MAX = 1024;
constexpr mx_u64_t MX_DIAG_EL3_MAX_INDEX = 30;

enum MxDiagRequest : mx_u64_t {
    MX_DIAG_SYSTEM = 0,   // index=0, MxDiagSystem; returns sizeof(MxDiagSystem)
    MX_DIAG_CPU = 1,      // index=logical CPU, MxDiagCpu; returns sizeof(MxDiagCpu)
    MX_DIAG_BOOT_LOG = 2, // index=byte offset, 1..READ_MAX bytes; returns count/EOF=0
    // Bounded EL3 queries.  index 0 = metadata; index 1 = request-window
    // registration canary; index 2 = read-only window reconnaissance; index 3 =
    // legacy completion probe (index 3) now returns NOT_IMPLEMENTED: Linux
    // requires a completed storage operation and defines no callback guard.
    MX_DIAG_EL3 = 3,      // indices 0..MAX_INDEX; 29/30 = notification-backed RPMB lifecycle
    MX_DIAG_HEALTH = 5,   // index=0, MxDiagHealth; read-only console telemetry
    MX_DIAG_RUNTIME = 4,  // index=first PID (1..limit), MxDiagRuntimeBatch
};

enum MxDiagBoard : mx_u32_t {
    MX_DIAG_BOARD_UNKNOWN = 0,
    MX_DIAG_BOARD_QEMU = 1,
    MX_DIAG_BOARD_ANNE = 2,
    MX_DIAG_BOARD_SWITCH = 3,
};

struct MxDiagSystem {
    mx_u32_t version;
    mx_u32_t board;
    mx_u32_t cpu_limit;
    mx_u32_t sampled_cpu;
    mx_u64_t online_mask;
    mx_u64_t current_el;
    mx_u64_t midr;
    mx_u64_t mpidr;
    mx_u64_t pfr0;
    mx_u64_t mmfr0;
    mx_u64_t sctlr;
    mx_u64_t tcr;
    mx_u64_t mair;
    mx_u64_t ctr;
    mx_u64_t counter_reported_hz;
    mx_u64_t counter_effective_hz;
    // The retained log is append-only. Read only this prefix to keep a report
    // finite while other CPUs continue logging. Capacity=0 means unsupported.
    mx_u64_t boot_log_size;
    mx_u64_t boot_log_capacity;
};
static_assert(sizeof(MxDiagSystem) == 128);

struct MxDiagCpu {
    mx_u32_t version;
    mx_u32_t cpu;
    mx_u64_t online;
    mx_u64_t mpidr;
    mx_u64_t boot_entry_el; // 0 means no entry recorded, rather than EL0 entry
    mx_u64_t monitor_ready;
    mx_u64_t context_switches;
    mx_u64_t timer_preempts;
    mx_u64_t timer_ticks;
    mx_u64_t ipis_handled;
};
static_assert(sizeof(MxDiagCpu) == 72);

constexpr mx_u32_t MX_DIAG_PROCESS_LIMIT = 64, MX_DIAG_RUNTIME_ROWS = 8;
// Fixed-size, pointer-free rows. Identity includes the PID generation so a
// consumer cannot charge a replacement process for the previous one's time.
struct MxDiagProcessRuntime {
    mx_u64_t identity, runtime_ns;
    mx_u32_t pid, principal_id, flags, reserved;
    char name[32];
};
static_assert(sizeof(MxDiagProcessRuntime) == 64);
struct MxDiagRuntimeBatch {
    mx_u32_t version, first_pid, count, next_pid; // next_pid=0 at end of table
    mx_u64_t sampled_ns, online_mask;
    MxDiagProcessRuntime rows[MX_DIAG_RUNTIME_ROWS];
};
static_assert(sizeof(MxDiagRuntimeBatch) == 544);
static_assert(sizeof(MxDiagRuntimeBatch) <= MX_DIAG_READ_MAX);

// Memory includes allocated and reserved pages in the allocator's RAM region.
// busy_ns is cumulative non-idle CPU time, including exited processes.
constexpr mx_u32_t MX_HEALTH_BATTERY_VOLTAGE = 1u << 0;
struct MxDiagHealth {
    mx_u32_t version, flags;
    mx_u64_t sampled_ns, online_mask, busy_ns, total_bytes, used_bytes;
    mx_u32_t battery_mv, reserved;
};
static_assert(sizeof(MxDiagHealth) == 56);

// The addresses are the values from the matching Hi6250 EMUI-8 DDR map.  They
// describe the layout that a future handoff would have to use; they are not a
// permission to access the shared area.  Query results are kept as four raw
// words because the secure firmware's standard metadata convention returns
// more than the first word on some releases.
struct MxDiagEl3 {
    mx_u32_t version;
    mx_u32_t available;
    mx_u64_t shared_base;
    mx_u64_t request_address;
    mx_u64_t request_size;
    mx_u64_t bl31_base;
    mx_u64_t bl31_size;
    mx_u64_t callback_offset_from_request; // Legacy ABI field; zero on Hi6250.
    mx_u64_t callback_address; // Linux defines no callback in rpmb_request.
    mx_u64_t call_count[4];
    mx_u64_t uid[4];
    mx_u64_t service_version[4];
    // Set only by the explicit el3-canary request.  Zero means the canary was
    // not requested; otherwise this is the raw x0 result from REQUEST_ADDR.
    mx_u64_t request_address_result;
};
static_assert(sizeof(MxDiagEl3) == 168);

enum MxDiagRpmbAction : mx_u32_t {
    MX_RPMB_STATUS, MX_RPMB_ARM, MX_RPMB_COUNTER, MX_RPMB_COMPLETE, MX_RPMB_RELEASE,
    MX_RPMB_ARM_EMMC,
    MX_RPMB_PROVISION_KEY,
    MX_RPMB_KEY_STATUS,
};
enum MxDiagRpmbResult : mx_u32_t {
    MX_RPMB_OK, MX_RPMB_BUSY, MX_RPMB_NO_REQUEST, MX_RPMB_REGISTRATION_FAILED,
    MX_RPMB_AMBIGUOUS_REQUEST, MX_RPMB_STALE_REQUEST, MX_RPMB_ALREADY_CALLED,
    MX_RPMB_COMPLETION_FAILED, MX_RPMB_BAD_ACTION,
    MX_RPMB_STORAGE_UNAVAILABLE,
    MX_RPMB_PROVISION_FAILED,
    MX_RPMB_ALREADY_PROVISIONED, MX_RPMB_PROVISION_UNCERTAIN,
    MX_RPMB_KEY_QUERY_FAILED,
};
enum MxRpmbProvisionStage : mx_u64_t {
    MX_RPMB_PROVISION_IDLE, MX_RPMB_PROVISION_PREFLIGHT, MX_RPMB_PROVISION_GET_FRAME,
    MX_RPMB_PROVISION_WAIT_FRAME, MX_RPMB_PROVISION_PROGRAM, MX_RPMB_PROVISION_VERIFY,
    MX_RPMB_PROVISION_DONE,
};
// Index 29. Only action is input; all other fields are kernel observations.
// ARM uses error completion; ARM_EMMC reads EXT_CSD and services real frames.
struct MxDiagEl3Rpmb {
    mx_u32_t version, action, result, armed;
    mx_u64_t events, event_cpu;
    mx_u64_t registering, registered, registration_result;
    mx_u64_t trigger_started, trigger_inflight, trigger_returned, trigger_result;
    mx_u64_t done_started, done_inflight, done_returned, done_result;
    mx_u64_t completion_error;
    mx_u64_t func_id, request_state, frame_type, key_status, request_status, exception_status;
    mx_u64_t storage_enabled, completed_events;
    mx_u64_t key_state; // 0 unknown, 1 unprogrammed, 2 counter query succeeded
    mx_u64_t provision_stage, provision_pending;
    mx_u64_t key_smc_result, key_transport_result, key_response_result, key_counter;
};
static_assert(sizeof(MxDiagEl3Rpmb) == 248);

// Read-only raw snapshots of the RPMB window. These legacy field names do not
// establish Hi6250 callback/status semantics; Linux defines no such callback.
// Nothing is written and no SMC is issued by this query.
struct MxDiagEl3Recon {
    mx_u32_t version;
    mx_u32_t reserved;
    mx_u64_t window_base;
    mx_u64_t window_size;
    mx_u64_t base_6000;
    mx_u64_t slot_6c30;
    mx_u64_t slot_6c38;
    mx_u64_t slot_6c40;
    mx_u64_t slot_6c48;
    mx_u64_t status_6c42; // Raw u16; not Linux's rpmb_request_status (+0x6a22).
    mx_u64_t bl31_lo;
    mx_u64_t bl31_hi;
    mx_u64_t ptr_count;
    mx_u64_t ptr_slots[8][2];
    // Breadcrumbs written by hazard probes (magic 0x454C3350 in the high word);
    // placed in the DRAM-retained request window so a reset still leaves them.
    // +0x6900..+0x691f overlaps Linux's HMAC buffer; research use only.
    mx_u64_t crumb[4];
    mx_u64_t isar0; // ID_AA64ISAR0_EL1 (crypto extensions etc.)
};
static_assert(sizeof(MxDiagEl3Recon) == 264);

// Legacy completion-probe record retained for ABI compatibility. Index 3 now
// returns NOT_IMPLEMENTED without touching the request window or calling BL31.
struct MxDiagEl3Done {
    mx_u32_t version;
    mx_u32_t aborted; // 0 = call made; 1 = registration failed; 2 = guard refused
    mx_u64_t registered; // REQUEST_ADDR raw x0
    mx_u64_t slot_pre;
    mx_u64_t status_pre;
    mx_u64_t slot_after_write;
    mx_u64_t status_after_write;
    mx_u64_t call_x0;
    mx_u64_t call_x1;
    mx_u64_t call_x2;
    mx_u64_t call_x3;
    mx_u64_t slot_post;
    mx_u64_t status_post;
    mx_u64_t cleared; // 1 if the marker was cleared while the call was made
    mx_u64_t tick_before;
    mx_u64_t tick_after;
};
static_assert(sizeof(MxDiagEl3Done) == 120);

// Read-only probe of secure-region readability from the normal world.
// Breadcrumbs land in the request window before each risky read.
struct MxDiagEl3Bl31 {
    mx_u32_t version;
    mx_u32_t aborted;
    mx_u64_t control_addr;
    mx_u64_t control_value;
    mx_u64_t bl31_addr;
    mx_u64_t bl31_read_ok;
    mx_u64_t bl31_words[4];
    mx_u64_t teeos_addr;
    mx_u64_t teeos_read_ok;
    mx_u64_t teeos_words[2];
    mx_u64_t crumb_stage;
    mx_u64_t tick_before;
    mx_u64_t tick_after;
};
static_assert(sizeof(MxDiagEl3Bl31) == 128);

// Candidate read-primitive probe on the 0xC500AA01 service (idx 0): replay the
// kernel"s own call, then a known-value oracle, then (only on a match) one
// 4-byte read of the BL31 mapping, executed at EL3.
struct MxDiagEl3Gv {
    mx_u32_t version;
    mx_u32_t aborted;
    mx_u64_t replay_ret;
    mx_u64_t replay_x1;
    mx_u64_t oracle_ret;
    mx_u64_t oracle_expected;
    mx_u64_t oracle_match;
    mx_u64_t bl31_ret;
    mx_u64_t crumb_stage;
    mx_u64_t tick_before;
    mx_u64_t tick_after;
};
static_assert(sizeof(MxDiagEl3Gv) == 80);

// Parameterized single call into the 0xC500AA01 service: idx 0..31 (cookie
// 0x55BBCCE0 | idx), caller-chosen x1/x2, raw returns.  For sweeping the
// sibling entries of the service faceid.c weaponizes.
struct MxDiagEl3Aa {
    mx_u32_t version;
    mx_u32_t aborted;
    mx_u64_t idx;
    mx_u64_t x1;
    mx_u64_t x2;
    mx_u64_t cookie;
    mx_u64_t r0;
    mx_u64_t r1;
    mx_u64_t r2;
    mx_u64_t r3;
    mx_u64_t crumb_stage;
};
static_assert(sizeof(MxDiagEl3Aa) == 80);

// Chunked read of the first 32 KiB of the BL31 share region (the dmss/efuse/
// freqdump/drm/getval sub-windows; normal-world readable by design).
struct MxDiagEl3Share {
    mx_u32_t version;
    mx_u32_t aborted;
    mx_u64_t offset;
    mx_u64_t len;
    mx_u8_t data[64];
};
static_assert(sizeof(MxDiagEl3Share) == 88);

// REQUEST_ADDR strictness probe (0xC600FF04 with a deliberately wrong address)
// plus immediate re-registration of the canonical window.
struct MxDiagEl3Ra {
    mx_u32_t version;
    mx_u32_t aborted;
    mx_u64_t target;
    mx_u64_t attempt_r0;
    mx_u64_t attempt_r1;
    mx_u64_t attempt_r2;
    mx_u64_t attempt_r3;
    mx_u64_t restore_r0;
    mx_u64_t restore_r1;
    mx_u64_t restore_r2;
    mx_u64_t restore_r3;
};
static_assert(sizeof(MxDiagEl3Ra) == 80);

// 0xC5009900 single call with a caller-chosen x1 (index-like arg).
struct MxDiagEl3Nn {
    mx_u32_t version;
    mx_u32_t aborted;
    mx_u64_t x1;
    mx_u64_t x2;
    mx_u64_t x3;
    mx_u64_t r0;
    mx_u64_t r1;
    mx_u64_t r2;
    mx_u64_t r3;
};
static_assert(sizeof(MxDiagEl3Nn) == 64);

// 0xC500CC00 single call with caller-chosen x1; crumbs before/after so a
// stall still leaves evidence after the watchdog reset.
struct MxDiagEl3Cc {
    mx_u32_t version;
    mx_u32_t aborted;
    mx_u64_t x1;
    mx_u64_t r0; mx_u64_t r1; mx_u64_t r2; mx_u64_t r3;
    mx_u64_t crumb_stage;
};
static_assert(sizeof(MxDiagEl3Cc) == 56);

// Twin CC00 call (zero-gap) with per-stage crumbs: cc2 <a> <b>.
struct MxDiagEl3Cc2 {
    mx_u32_t version;
    mx_u32_t aborted;
    mx_u64_t a; mx_u64_t b;
    mx_u64_t ra0; mx_u64_t ra1; mx_u64_t ra2; mx_u64_t ra3;
    mx_u64_t rb0; mx_u64_t rb1; mx_u64_t rb2; mx_u64_t rb3;
    mx_u64_t crumb_stage;
};
static_assert(sizeof(MxDiagEl3Cc2) == 96);

// Full 0xC500AA01 index sweep done in-kernel: rows[0..31] = plain calls for
// idx 0..31 (x1=x2=0); rows[32..35] = kernel-form calls for idx 7/a/e/f.
struct MxDiagEl3AaSweep {
    mx_u32_t version;
    mx_u32_t aborted;
    mx_u64_t rows[36];
    mx_u64_t crumb_stage;
};
static_assert(sizeof(MxDiagEl3AaSweep) == 304);

// SOCP descriptor experiment: write a 32-byte descriptor to the dmss area
// (0x34A21100), fire AA01 idx8 (submit) and idxA (completion), read back.
struct MxDiagEl3Socp {
    mx_u32_t version;
    mx_u32_t aborted;
    mx_u64_t mode; // 1=write only; 2=+idx8; 3=+idx8+idxA; 4=+idxA only
    mx_u64_t d[4];
    mx_u64_t p1;
    mx_u64_t p2;
    mx_u64_t r8[4];
    mx_u64_t rA[4];
    mx_u64_t readback[16];
    mx_u64_t crumb_stage;
};
static_assert(sizeof(MxDiagEl3Socp) == 264);

// Single 8-byte write test at share-base+off (deliberate; may fault).
struct MxDiagEl3Put {
    mx_u32_t version;
    mx_u32_t aborted;
    mx_u64_t off;
    mx_u64_t val;
    mx_u64_t readback;
    mx_u64_t crumb_stage;
};
static_assert(sizeof(MxDiagEl3Put) == 40);

// Single read of the TEEOS region (0x36208000) from the normal world.
struct MxDiagEl3Teeos {
    mx_u32_t version;
    mx_u32_t aborted;
    mx_u64_t addr;
    mx_u64_t ok;
    mx_u64_t words[2];
    mx_u64_t crumb_stage;
};
static_assert(sizeof(MxDiagEl3Teeos) == 48);

// Fault-tolerant single-address read (el3-rprobe). The kernel arms a data-abort
// catch for exactly one load; a denied address reports instead of resetting.
struct MxDiagEl3Rprobe {
    mx_u32_t version;
    mx_u32_t ok;        // 1 = read completed, 0 = fault was caught
    mx_u64_t addr;      // in
    mx_u64_t value;     // out when ok
    mx_u64_t esr;       // out when !ok — fault status
    mx_u64_t far;       // out when !ok — faulting address
    mx_u64_t elr;       // out when !ok — faulting PC
    mx_u64_t faults;    // out — faults observed during the read
};
static_assert(sizeof(MxDiagEl3Rprobe) == 56);

// Fault-tolerant range scan (el3-rscan). Walks [base, base+length) in 8-byte
// steps; every step that faults is skipped and counted. Bounded to 256 KiB.
constexpr mx_u64_t MX_DIAG_EL3_RSCAN_MAX = 0x40000;
struct MxDiagEl3Rscan {
    mx_u32_t version;
    mx_u32_t aborted;
    mx_u64_t base;          // in
    mx_u64_t length;        // in (<= RSCAN_MAX, multiple of 8)
    mx_u64_t readable;      // out — 8-byte words read successfully
    mx_u64_t faults;        // out — words that raised a handled fault
    mx_u64_t seg_count;     // out — readable byte ranges recorded (<= 8)
    mx_u64_t first_count;   // out — first non-zero readable words recorded (<= 8)
    mx_u64_t esr_last;      // out — ESR of the last fault caught
    mx_u64_t segments[8][2];     // [start, end) byte ranges
    mx_u64_t first_values[8][2]; // [addr, value]
};
static_assert(sizeof(MxDiagEl3Rscan) == 320);

// Map-and-read probe (el3-rmap). Maps a page-aligned window into the kernel
// identity space and the current address space (Device attributes, read-only),
// then attempts one u64 read with the fault catch armed — used to find out
// whether a reserved region is NS-reachable once translation exists.
struct MxDiagEl3Rmap {
    mx_u32_t version;
    mx_u32_t aborted;
    mx_u64_t addr;           // in
    mx_u64_t size;           // in (0 => 0x1000; capped at 0x20000)
    mx_u64_t mapped_global;  // out — kernel root mapping result
    mx_u64_t mapped_current; // out — current address-space mapping result
    mx_u64_t ok;             // out — read completed
    mx_u64_t value;          // out when ok
    mx_u64_t esr;            // out when !ok
    mx_u64_t far;            // out when !ok
    mx_u64_t faults;         // out
};
static_assert(sizeof(MxDiagEl3Rmap) == 80);

// Deliberate poke of the 0xC600FF06 flow with a caller-chosen callback slot
// (wedge-tolerant research probe; crumbs drop before the call).
struct MxDiagEl3Poke {
    mx_u32_t version;
    mx_u32_t aborted;
    mx_u64_t cb;
    mx_u64_t registered;
    mx_u64_t slot_pre;
    mx_u64_t slot_written;
    mx_u64_t status_set;
    mx_u64_t call_x0;
    mx_u64_t call_x1;
    mx_u64_t call_x2;
    mx_u64_t call_x3;
    mx_u64_t slot_post;
    mx_u64_t status_post;
    mx_u64_t tick_before;
    mx_u64_t tick_after;
    mx_u64_t crumb_stage;
};
static_assert(sizeof(MxDiagEl3Poke) == 120);

// Generic secure call: caller-chosen (fid, x1, x2, x3), raw returns.
struct MxDiagEl3Svc {
    mx_u32_t version;
    mx_u32_t aborted;
    mx_u64_t fid;
    mx_u64_t x1;
    mx_u64_t x2;
    mx_u64_t x3;
    mx_u64_t x4;
    mx_u64_t x5;
    mx_u64_t x6;
    mx_u64_t x7;
    mx_u64_t r0;
    mx_u64_t r1;
    mx_u64_t r2;
    mx_u64_t r3;
    mx_u64_t crumb_stage;
};
static_assert(sizeof(MxDiagEl3Svc) == 112);

// Window reader: 64 bytes at RPMB window (0x34A29000) + off.
struct MxDiagEl3Win {
    mx_u32_t version;
    mx_u32_t aborted;
    mx_u64_t off;
    mx_u64_t words[8];
    mx_u64_t crumb_stage;
};
static_assert(sizeof(MxDiagEl3Win) == 88);

// Window writer: single u64 at RPMB window (0x34A29000) + off.
struct MxDiagEl3Wput {
    mx_u32_t version;
    mx_u32_t aborted;
    mx_u64_t off;
    mx_u64_t val;
    mx_u64_t readback;
    mx_u64_t crumb_stage;
};
static_assert(sizeof(MxDiagEl3Wput) == 40);

// Window memset: fill [off, off+len) of the RPMB window with an 8-byte pattern.
struct MxDiagEl3Wmemset {
    mx_u32_t version;
    mx_u32_t aborted;
    mx_u64_t off;
    mx_u64_t len;
    mx_u64_t val;
    mx_u64_t crumb_stage;
};
static_assert(sizeof(MxDiagEl3Wmemset) == 40);

// Scan a readable region for u64 values within [lo, hi] (pointer hunting).
struct MxDiagEl3Scan {
    mx_u32_t version;
    mx_u32_t aborted;
    mx_u64_t base;
    mx_u64_t len;
    mx_u64_t lo;
    mx_u64_t hi;
    mx_u64_t count;
    mx_u64_t hits[20][2];
    mx_u64_t crumb_stage;
};
static_assert(sizeof(MxDiagEl3Scan) == 376);

// Multi-region scanner: up to 8 ranges, pointer hunt w/ up to 32 hits.
struct MxDiagEl3Scan2 {
    mx_u32_t version;
    mx_u32_t aborted;
    mx_u64_t nranges;
    mx_u64_t ranges[8][2];
    mx_u64_t lo;
    mx_u64_t hi;
    mx_u64_t count;
    mx_u64_t hits[32][3];
    mx_u64_t crumb_stage;
};
static_assert(sizeof(MxDiagEl3Scan2) == 944);
