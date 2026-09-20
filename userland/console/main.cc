// userland/console/main.cc — io.console service.
//
// Spec ref: §5.11 UART, §7.4 io.console
//
// Boot handshake:
//   init fills a ConsoleBootConfig page and grants it as cap slot 0.
//   Console maps the config, waits for magic, then maps UART/doorbell/rings
//   using the cap handles stored in the config.  Ring count is dynamic.

#include "mochi/syscall.h"
#include "mochi/ring.h"
#include "mochi/log.h"
#include "mochi/console_config.h"
#include "mochi/boot.h"
#include "mochi/terminal.h"

constexpr uint32_t kLogRingOrder    = 6;

constexpr uint64_t kVaConsoleConfig = 0x3000'0000ull;
constexpr uint64_t kVaUart          = 0x2000'0000ull;
constexpr uint64_t kVaLogRingBase   = 0x2001'0000ull;
constexpr uint64_t kVaTtyMirrorRing = 0x2040'0000ull;

// ---- UART helpers ----------------------------------------------------------

static void uart_putchar(volatile uint32_t* base, char c) {
    while (base[0x18 / 4] & (1u << 5)) { }
    base[0x00 / 4] = static_cast<uint32_t>(static_cast<unsigned char>(c));
}

static void uart_puts(volatile uint32_t* base, const char* s, uint32_t len) {
    for (uint32_t i = 0; i < len; ++i) uart_putchar(base, s[i]);
}

// ---- Cap/mapping helpers ---------------------------------------------------

template <typename T>
static T* wait_map(CapHandle cap, int64_t expected_type, uint64_t va, uint32_t rights) {
    while (true) {
        if (sys_cap_identify(cap) == expected_type) {
            if (sys_map_page(cap, va, rights) == 0) return reinterpret_cast<T*>(va);
        }
        sys_yield();
    }
}

static void wait_cap(CapHandle cap, int64_t expected_type) {
    while (sys_cap_identify(cap) != expected_type) {
        sys_yield();
    }
}

static RingPage* log_ring_at(uint32_t index) {
    return reinterpret_cast<RingPage*>(kVaLogRingBase +
                                       (static_cast<uint64_t>(index) * 0x10000ull));
}

// ---- Ring drain / sleep ----------------------------------------------------

static void mirror_bytes(const mochi::terminal::OutputSink& mirror,
                         const char* data, uint32_t len) {
    if (!data || len == 0) return;
    (void)mirror.try_write_bytes(data, len);
}

static bool drain_one(volatile uint32_t* uart, RingPage* ring,
                      const mochi::terminal::OutputSink& mirror) {
    bool any = false;
    RingSlot slot{};
    while (ring_pop(ring, kLogRingOrder, &slot)) {
        const char* bytes = reinterpret_cast<const char*>(slot.payload);
        uart_puts(uart, bytes, slot.length);
        mirror_bytes(mirror, bytes, slot.length);
        any = true;
    }
    return any;
}

static bool drain_all(volatile uint32_t* uart, uint32_t ring_count,
                      const mochi::terminal::OutputSink& mirror) {
    bool any = false;
    for (uint32_t i = 0; i < ring_count; ++i) {
        any = drain_one(uart, log_ring_at(i), mirror) || any;
    }
    return any;
}

static void publish_sleep(uint64_t counter, uint32_t ring_count) {
    for (uint32_t i = 0; i < ring_count; ++i) {
        RingPage* ring = log_ring_at(i);
        ring->flags.doorbell_counter_snapshot = counter;
        __atomic_store_n(&ring->flags.receiver_about_to_sleep,
                         1u,
                         __ATOMIC_RELEASE);
    }
}

static void clear_sleep(uint32_t ring_count) {
    for (uint32_t i = 0; i < ring_count; ++i) {
        RingPage* ring = log_ring_at(i);
        __atomic_store_n(&ring->flags.receiver_about_to_sleep,
                         0u,
                         __ATOMIC_RELEASE);
    }
}

static void uart_log(volatile uint32_t* uart, mochi::LogLevel level,
                     const char* component, const char* msg) {
    char line[256];
    bool truncated = false;
    const uint32_t len = mochi::log_format_line(line,
                                                sizeof(line),
                                                level,
                                                component,
                                                msg,
                                                &truncated);
    (void)truncated;
    uart_puts(uart, line, len);
}

// ---- Entry -----------------------------------------------------------------

extern "C" [[noreturn]] void _start() {
    // Slot 0 is the generic BootInfo page. The console-specific config is a
    // named arena entry inside it, so adding terminal channels later does not
    // require fixed cap-slot changes in this service.
    const MxBootInfo* boot = mochi::boot::map();
    const CapHandle config_cap = mochi::boot::require_handle(boot, "console.config");

    auto* cfg = wait_map<ConsoleBootConfig>(config_cap, kCapTypeArena,
                                            kVaConsoleConfig,
                                            RIGHT_READ | RIGHT_WRITE);

    // Wait for init to finish filling the service-specific config.
    while (__atomic_load_n(&cfg->magic, __ATOMIC_ACQUIRE) != ConsoleBootConfig::kMagic) {
        sys_yield();
    }

    const uint32_t ring_count = cfg->total_ring_count;

    // Map UART using cap handle from config.
    auto* uart = wait_map<volatile uint32_t>(static_cast<CapHandle>(cfg->uart_cap),
                                             kCapTypeUart,
                                             kVaUart,
                                             RIGHT_READ | RIGHT_WRITE);

    // Wait for doorbell.
    wait_cap(static_cast<CapHandle>(cfg->log_doorbell_cap), kCapTypeDoorbell);
    const CapHandle log_doorbell = static_cast<CapHandle>(cfg->log_doorbell_cap);

    // Map log rings.
    const uint32_t mapped = ring_count < ConsoleBootConfig::kMaxRings
                                ? ring_count
                                : ConsoleBootConfig::kMaxRings;
    for (uint32_t i = 0; i < mapped; ++i) {
        (void)wait_map<RingPage>(static_cast<CapHandle>(cfg->log_ring_caps[i]),
                                 kCapTypeSharedRing,
                                 kVaLogRingBase + (static_cast<uint64_t>(i) * 0x10000ull),
                                 RIGHT_READ | RIGHT_WRITE);
    }

    mochi::terminal::OutputSink tty_mirror{};
    tty_mirror.order = kLogRingOrder;
    const MxBootEntry* mirror_entry =
        mochi::boot::find_type(boot, "tty.mirror.ring", MX_BOOT_ENTRY_RING);
    if (mirror_entry && mirror_entry->handle != kNullCap &&
        sys_map_page(mirror_entry->handle, kVaTtyMirrorRing,
                     RIGHT_READ | RIGHT_WRITE) == 0) {
        tty_mirror.ring = reinterpret_cast<RingPage*>(kVaTtyMirrorRing);
        tty_mirror.order = mirror_entry->aux0
            ? static_cast<uint32_t>(mirror_entry->aux0)
            : kLogRingOrder;
        tty_mirror.doorbell = mochi::boot::handle(boot, "tty.mirror.doorbell");
    }

    (void)drain_all(uart, mapped, tty_mirror);
    uart_log(uart, mochi::LogLevel::Info, "io.console", "ready");

    mochi::boot::ready(boot);
    while (true) {
        if (drain_all(uart, mapped, tty_mirror)) {
            continue;
        }

        const uint64_t counter = sys_doorbell_read(log_doorbell);
        publish_sleep(counter, mapped);

        if (drain_all(uart, mapped, tty_mirror)) {
            clear_sleep(mapped);
            continue;
        }

        const int64_t rc = sys_wait_doorbell(log_doorbell, counter);
        clear_sleep(mapped);
        if (rc < 0) {
            sys_yield();
        }
    }
}
