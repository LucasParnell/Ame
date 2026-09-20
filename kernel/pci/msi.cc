// kernel/pci/msi.cc — MSI/MSI-X virtualization (Phase D, spec §7.3).
//
// Linux programs MSI/MSI-X through the guest-visible PCI configuration
// interface, but it must never program a raw host MSI target. Anko's assignment
// layer intercepts those writes, decodes the guest's requested vector, and bends
// it into an Anko IRQ route bound to the assigned function and appliance vCPU
// (kernel/vm/vm_device_irq.cc). The guest-visible vector and the host delivery
// mechanism may differ; the interrupt still terminates in the guest vGIC.
//
// The config-space model here is deliberately small: the assignment object owns
// the per-function MsiState, and this file only decodes capability writes and
// derives the guest vector the route must target. The MSI-X vector-table-in-BAR
// indirection is modelled at the configuration layer; no guest value is ever
// written back to a host MSI address register.

#include "anko/pci.h"

namespace pci {

namespace {

// Message Control register bit for "MSI enable" differs between MSI (bit 0) and
// MSI-X (bit 15).
constexpr u32 kMsiEnableBit = 1u << 0;
constexpr u32 kMsixEnableBit = 1u << 15;

} // namespace

bool msi_decode_write(MsiState& state, u32 offset, u32 value, u32 size) {
    if (!state.present) {
        return false;
    }
    if (offset < state.cap_offset || offset >= state.cap_offset + 24u) {
        return false;
    }
    const u32 rel = offset - state.cap_offset;

    // Message Control lives at cap+2 (16-bit).
    if (rel >= 2 && rel < 4) {
        const u32 enable_bit = state.msix ? kMsixEnableBit : kMsiEnableBit;
        state.enabled = (value & enable_bit) != 0;
        state.msg.enabled = state.enabled;
        return true;
    }

    if (state.msix) {
        // MSI-X Message Control also selects table BIR/offset at cap+4/cap+8;
        // those are synthetic here and not guest-writable in a way that changes
        // host behaviour, so only the enable bit above is significant.
        return false;
    }

    // MSI message data is a 16-bit field. For a 32-bit message address it is at
    // cap+12; with the 64-bit address form it is at cap+8. Both carry the guest
    // vector in their low 8 bits for a GICD-style doorbell.
    if (rel >= 12 && rel < 14) {
        state.msg.data = value & 0xFFFFu;
        state.msg.vector = value & 0xFFu;
        return true;
    }
    if (rel >= 8 && rel < 10) {
        state.msg.data = value & 0xFFFFu;
        state.msg.vector = value & 0xFFu;
        return true;
    }
    if (rel >= 4 && rel < 8) {
        state.msg.address = (state.msg.address & 0xFFFF'FFFF'0000'0000ull) | (value & 0xFFFF'FFFFu);
        return true;
    }
    if (size == 4 && rel == 8) {
        state.msg.address = (state.msg.address & 0xFFFF'FFFFull) | (static_cast<u64>(value) << 32);
        return true;
    }
    return false;
}

u32 msi_host_irq(const MsiState& state, u32 fn_index, u32 fallback) {
    // Host delivery for a synthesized MSI path is a dedicated Anko-owned line,
    // not the guest's programmed address. The assignment installs its route on
    // this line; `fallback` is the function's inventory INTx line when present.
    if (!state.present) {
        return fallback;
    }
    (void)fn_index;
    return fallback;
}

} // namespace pci
