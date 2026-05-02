// userland/console/main.cc — io.console service.
//
// Spec ref: §5.11 UART, §7.4 io.console
//
// V1 note:
//   After printing the early markers, console sleeps on its log-ring doorbell
//   instead of yield-polling an empty ring.

#include "mochi/syscall.h"
#include "mochi/ring.h"
#include "mochi/log.h"

constexpr uint32_t kLogRingOrder = 6;
constexpr uint32_t kKernelLogRingCount = 4;
constexpr uint32_t kServiceLogRingCount = 7; // init + manifest services
constexpr uint32_t kLogRingCount = kKernelLogRingCount + kServiceLogRingCount;
constexpr int64_t kCapTypeSharedRing = 4;
constexpr int64_t kCapTypeDoorbell = 5;
constexpr int64_t kCapTypeUart = 9;

constexpr CapHandle cap_slot(uint32_t idx) {
    return (static_cast<CapHandle>(1) << 32) | idx;
}

constexpr CapHandle kCapUart        = cap_slot(0);
constexpr CapHandle kCapLogDoorbell = cap_slot(1);
constexpr uint32_t kCapLogRingBase = 2;
constexpr uint32_t RIGHT_READ  = 1u << 0;
constexpr uint32_t RIGHT_WRITE = 1u << 1;
constexpr uint64_t kVaUart = 0x5000'0000ull;
constexpr uint64_t kVaLogRingBase = 0x5001'0000ull;

static void uart_putchar(volatile uint32_t* base, char c) {
    while (base[0x18 / 4] & (1u << 5)) { }
    base[0x00 / 4] = static_cast<uint32_t>(static_cast<unsigned char>(c));
}

static void uart_puts(volatile uint32_t* base, const char* s, uint32_t len) {
    for (uint32_t i = 0; i < len; ++i) uart_putchar(base, s[i]);
}

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

struct LogInput {
    RingPage* ring;
};

static bool drain_one(volatile uint32_t* uart, RingPage* ring) {
    bool any = false;
    RingSlot slot{};
    while (ring_pop(ring, kLogRingOrder, &slot)) {
        uart_puts(uart, reinterpret_cast<const char*>(slot.payload), slot.length);
        any = true;
    }
    return any;
}

static bool drain_all(volatile uint32_t* uart, LogInput* inputs, uint32_t count) {
    bool any = false;
    for (uint32_t i = 0; i < count; ++i) {
        any = drain_one(uart, inputs[i].ring) || any;
    }
    return any;
}

static void publish_sleep(LogInput* inputs, uint32_t count, uint64_t counter) {
    for (uint32_t i = 0; i < count; ++i) {
        inputs[i].ring->flags.doorbell_counter_snapshot = counter;
        __atomic_store_n(&inputs[i].ring->flags.receiver_about_to_sleep,
                         1u,
                         __ATOMIC_RELEASE);
    }
}

static void clear_sleep(LogInput* inputs, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        __atomic_store_n(&inputs[i].ring->flags.receiver_about_to_sleep,
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

extern "C" [[noreturn]] void _start() {
    auto* uart = wait_map<volatile uint32_t>(kCapUart, kCapTypeUart,
                                             kVaUart, RIGHT_READ | RIGHT_WRITE);
    wait_cap(kCapLogDoorbell, kCapTypeDoorbell);

    LogInput inputs[kLogRingCount]{};
    for (uint32_t i = 0; i < kLogRingCount; ++i) {
        inputs[i].ring = wait_map<RingPage>(cap_slot(kCapLogRingBase + i),
                                            kCapTypeSharedRing,
                                            kVaLogRingBase + (static_cast<uint64_t>(i) * 0x10000ull),
                                            RIGHT_READ | RIGHT_WRITE);
    }

    (void)drain_all(uart, inputs, kLogRingCount);
    uart_log(uart, mochi::LogLevel::Info, "io.console", "hello world");
    uart_log(uart, mochi::LogLevel::Info, "io.console", "CONSOLE OK");

    while (true) {
        if (drain_all(uart, inputs, kLogRingCount)) {
            continue;
        }

        const uint64_t counter = sys_doorbell_read(kCapLogDoorbell);
        publish_sleep(inputs, kLogRingCount, counter);

        if (drain_all(uart, inputs, kLogRingCount)) {
            clear_sleep(inputs, kLogRingCount);
            continue;
        }

        const int64_t rc = sys_wait_doorbell(kCapLogDoorbell, counter);
        clear_sleep(inputs, kLogRingCount);
        if (rc < 0) {
            sys_yield();
        }
    }
}
