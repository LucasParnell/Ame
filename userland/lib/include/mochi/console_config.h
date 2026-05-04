#pragma once
// mochi/console_config.h — boot handshake page between init and io.console.
//
// init fills this struct and grants the arena cap as console's first cap (slot 0).
// io.console maps it, waits for magic, then reads ring count and cap handles.

#include <cstdint>

struct ConsoleBootConfig {
    static constexpr uint32_t kMagic   = 0x434F4E46u; // 'CONF'
    static constexpr uint16_t kVersion = 1u;
    static constexpr uint32_t kMaxRings = 32u;

    uint32_t magic;             // written last by init; console spins until set
    uint16_t version;
    uint16_t flags;             // reserved, 0
    uint32_t total_ring_count;  // kernel rings + init ring + service rings
    uint32_t reserved0;

    uint64_t uart_cap;          // CapHandle of UART cap in console's table
    uint64_t log_doorbell_cap;  // CapHandle of log doorbell in console's table

    uint64_t log_ring_caps[kMaxRings]; // CapHandles of each log ring in console's table
};
static_assert(sizeof(ConsoleBootConfig) <= 4096, "ConsoleBootConfig must fit in one page");
