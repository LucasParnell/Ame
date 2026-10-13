#pragma once
// mochi/terminal.h — programmer-facing terminal views over a mochi::Ring.
//
// Terminal traffic is intentionally separate from log rings: logs are diagnostic
// records, terminal bytes are user/session data. Each view below is a Ring (a
// mapped ring page, its order, and the doorbell that wakes its consumer) with a
// fixed terminal opcode. The byte-run splitting, the queue handling and the
// doorbell protocol live in Ring, so a sink and a source differ only in the
// opcode they name — and every Ring operation stays available on them.

#include "mochi/endpoint.h"
#include "midix/abi/protocols/terminal.h"

namespace mochi::terminal {

// A terminal byte endpoint is a Ring; the name is kept for the call sites.
using ByteEndpoint = Ring;

// Write side: bytes out to the display (msh/io.console -> tty.fb0).
struct OutputSink : ByteEndpoint {
    bool try_write_bytes(const void* data, uint32_t len) const {
        return valid() && push_bytes(MX_TERM_OUTPUT_BYTES, data, len) == len;
    }

    bool write_bytes_spinning(const void* data, uint32_t len,
                              uint32_t max_yields_per_chunk = 1024) const {
        return valid() && push_bytes(MX_TERM_OUTPUT_BYTES, data, len, max_yields_per_chunk) == len;
    }
};

// Read side: the rendered display byte stream (io.console/tty.fb0 <- msh).
struct OutputSource : ByteEndpoint {
    bool try_read(RingSlot* out) const {
        if (!out || !ring) return false;
        if (!try_pop(out)) return false;
        return message_valid(*out) && out->type == MX_TERM_OUTPUT_BYTES;
    }

    bool wait_read(RingSlot* out) const {
        if (!out || !valid()) return false;
        for (;;) {
            if (!wait_pop(out)) return false;
            if (message_valid(*out) && out->type == MX_TERM_OUTPUT_BYTES) return true;
        }
    }
};

// Read side: input bytes headed for the shell (shell <- serial bridge).
struct InputSource : ByteEndpoint {
    bool try_read(RingSlot* out) const {
        if (!out || !ring) return false;
        if (!try_pop(out)) return false;
        return message_valid(*out) && out->type == MX_TERM_INPUT_BYTES;
    }
};

// Write side: input bytes into the shell (ui.input/tty.fb0 -> msh).
struct InputSink : ByteEndpoint {
    bool try_write_bytes(const void* data, uint32_t len) const {
        return valid() && push_bytes(MX_TERM_INPUT_BYTES, data, len) == len;
    }

    bool write_input_byte_spinning(char ch, uint32_t max_yields) const {
        return valid() && push_bytes(MX_TERM_INPUT_BYTES, &ch, 1, max_yields) == 1;
    }
};

// A full-duplex terminal session (the shell's view): input and output rings on
// one SPSC endpoint, each addressed as a Ring.
struct Terminal {
    mochi::SpscEndpoint endpoint;

    bool valid() const { return endpoint.valid(); }

    bool try_write_bytes(const void* data, uint32_t len) const {
        return endpoint.valid() && endpoint.tx().push_bytes(MX_TERM_OUTPUT_BYTES, data, len) == len;
    }

    bool write_bytes_spinning(const void* data, uint32_t len,
                              uint32_t max_yields_per_chunk = 1024) const {
        return endpoint.valid() &&
               endpoint.tx().push_bytes(MX_TERM_OUTPUT_BYTES, data, len, max_yields_per_chunk) == len;
    }

    bool try_read(RingSlot* out) const {
        if (!endpoint.try_recv(out)) return false;
        return out && message_valid(*out) && (out->type == MX_TERM_INPUT_BYTES || out->type == MX_TERM_CONTROL);
    }

    bool wait_read(RingSlot* out) const {
        for (;;) {
            if (!endpoint.wait_recv(out)) return false;
            if (out && message_valid(*out) && (out->type == MX_TERM_INPUT_BYTES || out->type == MX_TERM_CONTROL)) return true;
        }
    }
};

} // namespace mochi::terminal
