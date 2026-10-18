#include "rpmb_diagnostics.h"
#include "rpmb.h"
#include "rpmb_provisioning.h"
#include "anko/spinlock.h"

namespace anne::rpmb_diag {
namespace {
constexpr u32 kExclusive = 1u << 31;
u32 gate;
kernel::SpinLock lock{};
MxDiagEl3Rpmb state{};
u64 notifications, notification_cpu, baseline;
Storage active_storage{};

void barrier() {
#if defined(__aarch64__) && !defined(ANKO_HOST_TEST)
    asm volatile("dsb sy" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}
u64 load_le(const volatile u8* p, unsigned n) {
    u64 v = 0;
    for (unsigned i = 0; i < n; ++i) v |= u64(p[i]) << (8 * i);
    return v;
}
void report(u32 action, u32 result, MxDiagEl3Rpmb& out, const volatile u8* request) {
    state.events = state.armed ? __atomic_load_n(&notifications, __ATOMIC_ACQUIRE) - baseline : 0;
    state.event_cpu = __atomic_load_n(&notification_cpu, __ATOMIC_RELAXED);
    // Capture the request before completion; firmware may reuse it after FF06.
    if (state.registered && state.events > state.completed_events && !state.done_inflight &&
        (!state.done_started || state.storage_enabled)) {
        barrier();
        state.func_id = load_le(request + rpmb::kRequestInfoOffset, 4);
        state.request_state = load_le(request + rpmb::kRequestInfoOffset + 20, 4);
        state.frame_type = (u64(request[510]) << 8) | request[511];
        state.key_status = load_le(request + rpmb::kKeyStatusOffset, 2);
        state.request_status = load_le(request + rpmb::kRequestStatusOffset, 2);
        state.exception_status = load_le(request + rpmb::kExceptionStatusOffset, 2);
    }
    out = state;
    out.version = MX_DIAG_VERSION;
    out.action = action;
    out.result = result;
}
}

ResearchAccess::ResearchAccess() {
    u32 value = __atomic_load_n(&gate, __ATOMIC_ACQUIRE);
    while (!(value & kExclusive)) {
        if (__atomic_compare_exchange_n(&gate, &value, value + 1, false,
                                         __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            acquired_ = true;
            break;
        }
    }
}
ResearchAccess::~ResearchAccess() {
    if (acquired_) __atomic_fetch_sub(&gate, 1u, __ATOMIC_RELEASE);
}
void notify_request(u32 cpu) {
    __atomic_store_n(&notification_cpu, u64(cpu), __ATOMIC_RELAXED);
    __atomic_fetch_add(&notifications, u64{1}, __ATOMIC_RELEASE);
}

bool producer_allowed(u64 fid) {
    if (fid != 0xc5000020ull) return false;
    kernel::LockGuard guard(lock);
    return state.armed && state.registered && state.storage_enabled &&
           !state.registering && !state.provision_pending;
}

void execute(u32 action, MxDiagEl3Rpmb& out, volatile u8* request, Smc call, const Storage* storage) {
    u64 fid = 0, x1 = 0;
    bool prepare_storage = false, service_storage = false;
    const bool provision_key = action == MX_RPMB_PROVISION_KEY;
    const bool key_query = action == MX_RPMB_KEY_STATUS;
    rpmb::KeyInfo observed_key{};
    {
        kernel::LockGuard guard(lock);
        auto reject = [&](u32 result) { report(action, result, out, request); };
        if (action == MX_RPMB_STATUS) { reject(MX_RPMB_OK); return; }
        if (state.provision_pending) { reject(MX_RPMB_BUSY); return; }
        if (action == MX_RPMB_ARM || action == MX_RPMB_ARM_EMMC || provision_key || key_query) {
            if ((action == MX_RPMB_ARM_EMMC || provision_key || key_query) &&
                (!storage || !storage->prepare || !storage->service || !storage->write_pending)) {
                reject(MX_RPMB_STORAGE_UNAVAILABLE); return;
            }
            if (provision_key && (!storage->provision_key || !storage->key_info || !storage->delay_ms)) {
                reject(MX_RPMB_STORAGE_UNAVAILABLE); return;
            }
            if (key_query && !storage->key_info) { reject(MX_RPMB_STORAGE_UNAVAILABLE); return; }
            u32 empty = 0;
            if (state.armed || !__atomic_compare_exchange_n(&gate, &empty, kExclusive, false,
                                                            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
                reject(MX_RPMB_BUSY); return;
            }
            barrier();
            if (!key_query && load_le(request + rpmb::kRequestStatusOffset, 2)) {
                __atomic_store_n(&gate, 0u, __ATOMIC_RELEASE);
                reject(MX_RPMB_STALE_REQUEST); return;
            }
            state = {};
            baseline = __atomic_load_n(&notifications, __ATOMIC_ACQUIRE);
            state.armed = 1;
            state.registering = 1;
            state.completion_error = rpmb::kErrorBlockDevice;
            prepare_storage = action == MX_RPMB_ARM_EMMC || provision_key || key_query;
            state.storage_enabled = prepare_storage;
            active_storage = prepare_storage ? *storage : Storage{};
            fid = rpmb::kRequestAddressSmc;
            x1 = reinterpret_cast<u64>(request); // Native window is identity mapped.
        } else if (action == MX_RPMB_COUNTER) {
            if (!state.armed || !state.registered || state.registering) { reject(MX_RPMB_BUSY); return; }
            if (state.trigger_started) { reject(MX_RPMB_ALREADY_CALLED); return; }
            if (__atomic_load_n(&notifications, __ATOMIC_ACQUIRE) != baseline) {
                reject(MX_RPMB_BUSY); return;
            }
            state.trigger_started = state.trigger_inflight = 1;
            fid = rpmb::kCounterSmc;
        } else if (action == MX_RPMB_COMPLETE) {
            if (!state.armed || !state.registered || state.registering) { reject(MX_RPMB_BUSY); return; }
            if (state.done_inflight || (state.done_started &&
                (!state.storage_enabled || !state.done_returned || state.done_result))) {
                reject(MX_RPMB_ALREADY_CALLED); return;
            }
            const u64 events = __atomic_load_n(&notifications, __ATOMIC_ACQUIRE) - baseline;
            const u64 pending = events - state.completed_events;
            if (!pending) { reject(MX_RPMB_NO_REQUEST); return; }
            if (pending != 1) { reject(MX_RPMB_AMBIGUOUS_REQUEST); return; }
            report(action, MX_RPMB_OK, out, request);
            state.done_started = state.done_inflight = 1;
            state.done_returned = 0;
            service_storage = state.storage_enabled;
            barrier();
            fid = rpmb::kRequestDoneSmc;
            x1 = rpmb::kErrorBlockDevice;
        } else if (action == MX_RPMB_RELEASE) {
            const u64 events = state.armed ? __atomic_load_n(&notifications, __ATOMIC_ACQUIRE) - baseline : 0;
            if (state.registering || state.trigger_inflight || state.done_inflight ||
                (events && (!state.done_returned || state.done_result || events != state.completed_events)) ||
                (state.storage_enabled && active_storage.write_pending())) {
                reject(MX_RPMB_BUSY); return;
            }
            // Releasing local ownership does not unregister the firmware buffer.
            state.armed = 0;
            if (__atomic_load_n(&gate, __ATOMIC_RELAXED) == kExclusive)
                __atomic_store_n(&gate, 0u, __ATOMIC_RELEASE);
            reject(MX_RPMB_OK); return;
        } else { reject(MX_RPMB_BAD_ACTION); return; }
    }
    // Match the stock worker's IRQ-enabled context. Kernel IRQ entry preserves
    // the syscall frame and defers scheduling until this syscall returns.
    // Keep FIQ/SError/debug masks unchanged; no lock spans the firmware call.
#if defined(__aarch64__) && !defined(ANKO_HOST_TEST)
    u64 saved_daif;
    asm volatile("mrs %0, daif\n msr daifclr, #2" : "=r"(saved_daif) :: "memory");
#endif
    if (action == MX_RPMB_ARM || action == MX_RPMB_ARM_EMMC || provision_key || key_query) {
        auto config = rpmb::kAnneConfig;
        if (prepare_storage && !active_storage.prepare(config)) {
#if defined(__aarch64__) && !defined(ANKO_HOST_TEST)
            asm volatile("msr daif, %0" :: "r"(kernel::host_interrupt_state(saved_daif)) : "memory");
#endif
            kernel::LockGuard guard(lock);
            state.registering = state.armed = state.storage_enabled = 0;
            __atomic_store_n(&gate, 0u, __ATOMIC_RELEASE);
            report(action, MX_RPMB_STORAGE_UNAVAILABLE, out, request);
            return;
        }
        if (prepare_storage && !provision_key && active_storage.key_info) observed_key = active_storage.key_info();
        if (!key_query) {
            const auto* bytes = reinterpret_cast<const u8*>(&config);
            for (usize i = 0; i < sizeof(config); ++i) request[i] = bytes[i];
        }
        barrier();
    }
    if (key_query) {
#if defined(__aarch64__) && !defined(ANKO_HOST_TEST)
        asm volatile("msr daif, %0" :: "r"(kernel::host_interrupt_state(saved_daif)) : "memory");
#endif
        kernel::LockGuard guard(lock);
        state.key_state = observed_key.present() ? 2 : observed_key.missing() ? 1 : 0;
        state.key_counter = observed_key.counter;
        state.key_transport_result = observed_key.error;
        state.key_response_result = (u64(observed_key.response) << 16) | observed_key.result;
        state.armed = state.registering = state.storage_enabled = 0;
        active_storage = {};
        __atomic_store_n(&gate, 0u, __ATOMIC_RELEASE);
        report(action, state.key_state ? MX_RPMB_OK : MX_RPMB_KEY_QUERY_FAILED, out, request);
        return;
    }
    if (service_storage) {
        x1 = active_storage.service(request);
        barrier();
        kernel::LockGuard guard(lock);
        state.completion_error = x1;
    }
    const u64 result = call(fid, x1, 0, 0);
    if (provision_key) {
        ProvisionResult outcome;
        if (result) {
            outcome.result = MX_RPMB_REGISTRATION_FAILED;
            outcome.transport = result;
        } else outcome = provision(request, active_storage, call);
#if defined(__aarch64__) && !defined(ANKO_HOST_TEST)
        asm volatile("msr daif, %0" :: "r"(kernel::host_interrupt_state(saved_daif)) : "memory");
#endif
        kernel::LockGuard guard(lock);
        state.registering = 0;
        state.registration_result = result;
        state.registered = result == 0;
        state.key_state = outcome.key.present() ? 2 : outcome.key.missing() ? 1 : 0;
        state.key_counter = outcome.key.counter;
        state.provision_stage = outcome.stage;
        state.provision_pending = outcome.pending;
        state.key_smc_result = outcome.smc_result;
        state.key_transport_result = outcome.transport;
        state.key_response_result = outcome.response_result;
        state.completion_error = outcome.transport;
        state.storage_enabled = 0;
        // A late FF05 producer may still own the buffer after a key-frame wait
        // timeout. Keep the gate closed; no guessed cancellation ABI exists.
        const u64 events = __atomic_load_n(&notifications, __ATOMIC_ACQUIRE) - baseline;
        state.armed = outcome.pending || events;
        if (events) state.provision_pending = 1;
        if (!state.armed) {
            active_storage = {};
            __atomic_store_n(&gate, 0u, __ATOMIC_RELEASE);
        }
        report(action, outcome.result, out, request);
        return;
    }
#if defined(__aarch64__) && !defined(ANKO_HOST_TEST)
    asm volatile("msr daif, %0" :: "r"(kernel::host_interrupt_state(saved_daif)) : "memory");
#endif
    {
        kernel::LockGuard guard(lock);
        u32 status = MX_RPMB_OK;
        if (action == MX_RPMB_ARM || action == MX_RPMB_ARM_EMMC) {
            state.registering = 0;
            state.registration_result = result;
            state.registered = result == 0;
            state.key_state = observed_key.present() ? 2 : observed_key.missing() ? 1 : 0;
            state.key_counter = observed_key.counter;
            state.key_transport_result = observed_key.error;
            state.key_response_result = (u64(observed_key.response) << 16) | observed_key.result;
            if (result) status = MX_RPMB_REGISTRATION_FAILED;
        } else if (action == MX_RPMB_COUNTER) {
            state.trigger_inflight = 0;
            state.trigger_returned = 1;
            state.trigger_result = result;
        } else {
            state.done_inflight = 0;
            state.done_returned = 1;
            state.done_result = result;
            if (!result) ++state.completed_events;
            if (result) status = MX_RPMB_COMPLETION_FAILED;
        }
        report(action, status, out, request);
    }
}
}
