#pragma once
// mochi/console_config.h — boot handshake page between init and io.console.
//
// This is a compatibility service-specific config page. The general process ABI
// is MxBootInfo in midix/abi/boot.h; io.console keeps this compact page because
// it is the first service brought up and needs to become useful before the rest
// of the userland ABI is online.

#include <cstdint>
#include "midix/abi/boot.h"

struct ConsoleBootConfig {
    static constexpr uint32_t kMagic   = 0x434F4E46u; // 'CONF'
    static constexpr uint16_t kVersion = 2u;
    static constexpr uint32_t kMaxRings = 32u;

    uint32_t magic;             // written last by init; console spins until set
    uint16_t version;
    uint16_t flags;             // reserved, 0
    uint32_t total_ring_count;  // kernel rings + init ring + service rings
    uint32_t reserved0;

    uint64_t uart_cap;          // CapHandle of UART cap in console's table
    uint64_t log_doorbell_cap;  // CapHandle of log doorbell in console's table

    // Optional terminal channel caps for the upcoming serial terminal. Zero
    // means absent. These are deliberately separate from log rings so typed
    // input never becomes kernel/log state.
    uint64_t term_input_ring_cap;      // io.console -> shell
    uint64_t term_output_ring_cap;     // shell -> io.console
    uint64_t term_input_doorbell_cap;  // wakes shell
    uint64_t term_output_doorbell_cap; // wakes io.console; may equal log_doorbell_cap

    uint64_t log_ring_caps[kMaxRings]; // CapHandles of each log ring in console's table
};
static_assert(sizeof(ConsoleBootConfig) <= 4096, "ConsoleBootConfig must fit in one page");
