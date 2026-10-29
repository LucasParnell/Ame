#include "scanout.h"
#include <cassert>
#include <map>
using namespace anne;
struct Io {
    std::map<u32, u32> registers;
    u32 programs = 0;
    u32 read(u32 offset) {
        return registers[offset];
    }
    void write(u32 offset, u32 value) {
        if (offset == scanout::kInterrupt) {
            registers[offset] &= ~value;
        } else {
            registers[offset] = value;
        }
        if (offset == scanout::kMutex && value == 1) {
            ++programs;
        }
    }
    void barrier() {}
};
static native_display::Pipeline pipeline() {
    native_display::Pipeline p{};
    p.index = 0;
    p.dma = {1,    5 << 3, static_cast<u32>(native_display::kLogoBase), 270, 0, 0, 269, 2279, 1,
             1079, 2279};
    p.dsi_power = p.dsi_phy = 1;
    p.dsi_width = 1080;
    p.dsi_height = 2280;
    p.mctl_enable = p.mctl_itf = p.mctl_dbuf = p.mctl_overlay = 1;
    p.overlay_size = p.overlay_end = p.layer_end[0] = (2279u << 16) | 1079;
    p.overlay_config = p.layer_config[0] = 1;
    p.channel_overlay_enable = 0x102;
    p.channel_select = 8;
    p.dfc_size = (1079u << 16) | 2279;
    return p;
}
int main() {
    Io io;
    scanout::State state;
    auto p = pipeline();
    auto invalid = p;
    invalid.layer_config[1] = 1;
    assert(!state.prepare(io, invalid)); // preserve other visible layers
    invalid = p;
    invalid.dma.control |= 1u << 8;
    assert(!state.prepare(io, invalid)); // no IOVA/physical-address confusion
    io.registers[scanout::kMutex] = 1;
    assert(!state.prepare(io, p)); // never take an already held hardware mutex
    io.registers[scanout::kMutex] = 0;
    assert(state.prepare(io, p));
    constexpr u64 address0 = 0x2000000, address1 = address0 + AME_GPU_DIAG_BYTES;
    assert(state.step(io, 0, address0, 0) == 1 && io.programs == 1);
    assert(state.step(io, 0, address0, 1) == 1 && io.programs == 1);
    io.registers[scanout::kInterrupt] = scanout::kStart;
    assert(state.step(io, 0, address0, 2) == 1 && state.active == -1); // stale active edge
    io.registers[scanout::kInterrupt] = scanout::kSync | scanout::kStart;
    assert(state.step(io, 0, address0, 3) == 1 && state.active == -1); // coalesced events
    assert(io.registers[0x52060] == address0 + 511 * 2048);            // FLIP_V last row
    assert(io.registers[0x5201c] == (p.dma.control | (2u << 9)));
    assert(io.registers[0x52064] == 128 && io.registers[0x5206c] == 65536);
    assert(state.active == -1); // readback/programming is not a retirement fence
    assert(state.step(io, 1, address1, 4) == 1 && state.active == -1);
    io.registers[scanout::kInterrupt] |= scanout::kStart;
    io.registers[scanout::kVstate] = scanout::kActive;
    assert(state.step(io, 0, address0, 5) == 0 && state.active == 0);
    assert(state.step(io, 1, address1, 6) == 1);
    io.registers[scanout::kInterrupt] = scanout::kSync;
    io.registers[scanout::kVstate] = 4;
    assert(state.step(io, 1, address1, 7) == 1 && state.active == 0);
    io.registers[scanout::kInterrupt] |= scanout::kStart;
    io.registers[scanout::kVstate] = scanout::kActive;
    assert(state.step(io, 1, address1, 8) == 0 && state.active == 1);
    assert(state.step(io, -1, 0, 9) == 1);
    io.registers[scanout::kInterrupt] = scanout::kSync;
    io.registers[scanout::kVstate] = 4;
    assert(state.step(io, -1, 0, 10) == 1);
    io.registers[scanout::kInterrupt] |= scanout::kStart;
    io.registers[scanout::kVstate] = scanout::kActive;
    assert(state.step(io, -1, 0, 11) == 0 && !state.live());
    for (u32 i = 0; i < scanout::kWords; ++i) {
        assert(io.registers[state.registers.offset[i]] == state.registers.console[i]);
    }
    assert(state.step(io, 0, address0, 12) == 1);
    io.registers[scanout::kInterrupt] = scanout::kSync;
    io.registers[scanout::kVstate] = 4;
    assert(state.step(io, 0, address0, 13) == 1);
    assert(state.step(io, 0, address0, 100000013) == -1 && state.live());
    assert(state.step(io, 1, address1, 100000014) == -1); // quarantined after timeout
    assert(state.step(io, -1, 0, 100000015) == 1);        // console must really be restored
    io.registers[scanout::kInterrupt] = scanout::kSync;
    io.registers[scanout::kVstate] = 4;
    assert(state.step(io, -1, 0, 100000016) == 1);
    io.registers[scanout::kInterrupt] |= scanout::kStart;
    io.registers[scanout::kVstate] = scanout::kActive;
    assert(state.step(io, -1, 0, 100000017) == 0 && !state.live());
    assert(state.step(io, 0, address0, 100000018) == 1);
    io.registers[scanout::kInterrupt] = scanout::kSync | scanout::kUnderflow;
    io.registers[scanout::kVstate] = scanout::kActive;
    assert(state.step(io, 0, address0, 100000019) == -1);
    assert(state.underflows == 1 && state.live());
    assert(state.step(io, 1, address1, 100000020) == -1);
    assert(state.step(io, -1, 0, 100000021) == 1);
    io.registers[scanout::kInterrupt] = scanout::kSync;
    assert(state.step(io, -1, 0, 100000022) == 0 && !state.live());
}
