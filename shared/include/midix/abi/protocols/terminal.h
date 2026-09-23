#pragma once
// Midix terminal (tty) protocol ABI.
//
// This is the contract between the console service (`io.console`), the
// framebuffer terminal (`tty.fb0`), and the shell (`msh`). It is deliberately a
// byte-stream protocol and data-oriented:
//
//   * INPUT_BYTES / OUTPUT_BYTES carry a run of raw bytes in the ring-slot
//     payload. `slot.length` is the run length and the slot opcode is the
//     direction, so there is no per-slot header and no byte is wasted on one. A
//     write longer than MX_TERM_INLINE_BYTES is split by the producer into
//     ordered slots on the same ring and reassembled by the consumer in order.
//   * RESIZE reports a new character-cell geometry.
//   * CONTROL carries an out-of-band control code, never user bytes.
//
// Terminal traffic is intentionally separate from the log rings: logs are
// diagnostic records, terminal bytes are user/session data.
//
// Transport envelope: MxRingSlot, exactly like every other Mochi ring. `type`
// is the opcode, `length` bounds the inline payload (48 bytes), and `seq` is the
// producer's running ring index. Every payload struct below fits the inline
// budget; the only place a byte run is unbounded is the byte-run length itself,
// which is capped at MX_TERM_INLINE_BYTES per slot.

#include "midix/abi/types.h"
#include "midix/abi/ring.h"

constexpr mx_u32_t MX_PROTOCOL_TERMINAL = 0x5445524Du; // 'TERM'
constexpr mx_u16_t MX_TERM_VERSION = 1;

// The control-ring frame for this protocol is the shared ring slot verbatim.
using MxTermFrame = MxRingSlot;
static_assert(sizeof(MxTermFrame) == 64, "terminal frame must be one ring slot");

// A terminal byte run uses the whole inline payload; the opcode and the slot
// length carry the framing, so no bytes are spent on a header.
constexpr mx_u32_t MX_TERM_INLINE_BYTES = MX_RING_INLINE_PAYLOAD;
static_assert(MX_TERM_INLINE_BYTES == 48, "terminal byte budget changed");

enum MxTermOpcode : mx_u32_t {
    // A run of input bytes flowing into the shell (tty.fb0/ui.input -> msh).
    MX_TERM_INPUT_BYTES  = 0x54494E01u,
    // A run of output bytes flowing out to the display (msh -> tty.fb0/io.console).
    MX_TERM_OUTPUT_BYTES = 0x544F5501u,
    // A character-cell geometry change (see MxTermResize).
    MX_TERM_RESIZE       = 0x54525301u,
    // An out-of-band control code, never user bytes (see MxTermControl).
    MX_TERM_CONTROL      = 0x54435401u,
};

// --- RESIZE -----------------------------------------------------------------
//
// The new character-cell geometry. Emitted by whichever end owns the visible
// grid; the reader reflows its cursor and line discipline to it. Reserved fields
// keep the record free for future metrics without a version bump.
struct MxTermResize {
    mx_u16_t cols;
    mx_u16_t rows;
    mx_u32_t reserved0;
};
static_assert(sizeof(MxTermResize) <= MX_TERM_INLINE_BYTES, "resize must fit one payload");
static_assert(sizeof(MxTermResize) == 8, "resize size drift");

// --- CONTROL ----------------------------------------------------------------
//
// A small fixed vocabulary so neither end uses magic bytes for out-of-band
// events. A code the reader does not know is dropped (never shown as bytes).
enum MxTermControlCode : mx_u32_t {
    MX_TERM_CONTROL_NONE     = 0,
    MX_TERM_CONTROL_FLOW_ON  = 1, // resume output
    MX_TERM_CONTROL_FLOW_OFF = 2, // suspend output (back-pressure)
    MX_TERM_CONTROL_EOF      = 3, // input stream ended
    MX_TERM_CONTROL_HANGUP   = 4, // peer detached
};

struct MxTermControl {
    mx_u32_t code;   // MxTermControlCode
    mx_u32_t arg0;   // code-specific, else 0
    mx_u32_t arg1;   // code-specific, else 0
    mx_u32_t reserved0;
};
static_assert(sizeof(MxTermControl) <= MX_TERM_INLINE_BYTES, "control must fit one payload");
static_assert(sizeof(MxTermControl) == 16, "control size drift");

// --- Compile-time invariants ------------------------------------------------

#if defined(__cplusplus)
// Every opcode value is unique; enumerated in one place so a copy/paste
// duplicate or a renumbered opcode fails the build in every translation unit.
constexpr mx_u32_t kMxTermOpcodeValues[] = {
    MX_TERM_INPUT_BYTES, MX_TERM_OUTPUT_BYTES, MX_TERM_RESIZE, MX_TERM_CONTROL,
};

constexpr bool mx_term_opcodes_unique() {
    constexpr mx_size_t count = sizeof(kMxTermOpcodeValues) / sizeof(kMxTermOpcodeValues[0]);
    for (mx_size_t i = 0; i < count; ++i) {
        for (mx_size_t j = i + 1; j < count; ++j) {
            if (kMxTermOpcodeValues[i] == kMxTermOpcodeValues[j]) {
                return false;
            }
        }
    }
    return true;
}
static_assert(mx_term_opcodes_unique(), "terminal opcode values must be unique");
#endif
