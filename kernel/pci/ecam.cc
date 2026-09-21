// Pure synthetic PCI configuration operations. No host MMIO is reachable here.
#include "anko/pci.h"

#include "libk/integer.h"

namespace pci {
namespace {
bool valid_access(u32 offset, u32 size, u32 limit) {
    return (size == 1 || size == 2 || size == 4) && offset < limit && size <= limit - offset &&
           (offset & (size - 1)) == 0;
}
u32 load(const u8* bytes, u32 offset, u32 size) {
    u32 value = 0;
    for (u32 i = 0; i < size; ++i) {
        value |= static_cast<u32>(bytes[offset + i]) << (i * 8);
    }
    return value;
}
void store(u8* bytes, u32 offset, u32 value) {
    for (u32 i = 0; i < 4; ++i) {
        bytes[offset + i] = static_cast<u8>(value >> (i * 8));
    }
}
} // namespace

bool decode_ecam(u64 offset, u32 size, EcamAddress* out) {
    if (!out || offset >= kGuestEcamSize ||
        !valid_access(static_cast<u32>(offset), size, kGuestEcamSize)) {
        return false;
    }
    *out = {static_cast<u32>((offset >> 12) & 7), static_cast<u32>(offset & 0xfff),
            (offset >> 15) == kGuestDevice};
    return true;
}

u32 config_read(const GuestConfig& cfg, u32 function, u32 offset, u32 size) {
    if ((size != 1 && size != 2 && size != 4) || (offset & (size - 1)) ||
        function >= kMaxAssignFns || !cfg.present[function]) {
        return 0xffffffffu;
    }
    // No extended capabilities: the portion of the 4 KiB ECAM access window
    // beyond this function's 256-byte config space reads as zero.
    if (offset >= kCfgSpaceBytes) {
        return 0;
    }
    // Bound the on-image access to config space: never let size run past the
    // 256-byte function header into the next synthetic record.
    if (!valid_access(offset, size, kCfgSpaceBytes)) {
        return 0xffffffffu;
    }
    if (offset >= kRegBar0 && offset < kRegBarEnd) {
        const u32 bar = (offset - kRegBar0) / 4;
        if (cfg.bar_probe[function] & (1u << bar)) {
            const u32 shift = (offset & 3) * 8;
            return (cfg.bar_mask[function][bar] >> shift) &
                   (size == 4 ? 0xffffffffu : ((1u << (size * 8)) - 1));
        }
    }
    return load(cfg.space[function], offset, size);
}

bool config_bar_write(GuestConfig& cfg, u32 function, u32 offset, u32 value, u32 size) {
    if (function >= kMaxAssignFns || !cfg.present[function] || size != 4 || (offset & 3) ||
        offset < kRegBar0 || offset >= kRegBarEnd) {
        return false;
    }
    const u32 bar = (offset - kRegBar0) / 4;
    if (value == kBarProbeValue) {
        cfg.bar_probe[function] |= static_cast<u8>(1u << bar);
        return true;
    }
    // Restore exactly the fixed, ownership-validated guest BAR. Relocation fails closed.
    if (value != load(cfg.space[function], offset, 4)) {
        return false;
    }
    cfg.bar_probe[function] &= static_cast<u8>(~(1u << bar));
    return true;
}

bool config_set_bar(GuestConfig& cfg, u32 function, u32 bar, u64 base, const PciBar& info) {
    if (function >= kMaxAssignFns || bar >= kMaxBars || info.size < kBarMinSize ||
        cfg.bar_mask[function][bar] || (cfg.bar_upper[function] & (1u << bar)) ||
        (info.size & (info.size - 1)) || (base & (info.size - 1)) || base > ~0ull - info.size) {
        return false;
    }
    const bool wide = info.type == BarType::Mmio64;
    if ((!wide && info.type != BarType::Mmio32) ||
        (wide && (bar + 1 >= kMaxBars || cfg.bar_mask[function][bar + 1])) ||
        (!wide && (base + info.size > kMmio32BarLimit))) {
        return false;
    }
    const u32 flags = (wide ? 4u : 0u) | (info.prefetchable ? 8u : 0u);
    const u64 mask = ~(info.size - 1);
    store(cfg.space[function], kRegBar0 + bar * 4, static_cast<u32>(base) | flags);
    cfg.bar_mask[function][bar] = (static_cast<u32>(mask) & kBarAddrMask) | flags;
    if (wide) {
        store(cfg.space[function], kRegBar0 + (bar + 1) * 4, static_cast<u32>(base >> 32));
        cfg.bar_mask[function][bar + 1] = static_cast<u32>(mask >> 32);
        cfg.bar_upper[function] |= static_cast<u8>(1u << (bar + 1));
    }
    return true;
}

bool bar_range_safe(const u8* host, const PciBar* bars, u32 bar, u64 start, u64 size) {
    if (!host || !bars || bar >= kMaxBars || !size || (start & 4095) || (size & 4095) ||
        start >= bars[bar].size || size > bars[bar].size - start) {
        return false;
    }
    u64 visited = 0;
    u32 offset = (host[6] & kStatusCapabilitiesList) ? host[kRegCapPtr] : 0;
    while (offset) {
        if (offset < kCfgCapMin || offset > kCfgCapMax || (offset & 3) ||
            (visited & (1ull << (offset / 4)))) {
            return false;
        }
        visited |= 1ull << (offset / 4);
        if (host[offset] == kCapIdMsix) {
            if (offset > kCfgSpaceBytes - 12) {
                return false;
            }
            const u32 vectors = (load(host, offset + kMsiCtrlOffset, 2) & kMsixVectorCountMask) + 1;
            for (u32 i = 0; i < 2; ++i) {
                const u32 field = load(host, offset + 4 + i * 4, 4);
                const u32 bir = field & kMsixBirMask;
                const u64 region = field & ~kMsixBirMask;
                const u64 bytes =
                    i == 0
                        ? static_cast<u64>(vectors) * kMsixTableEntryBytes
                        : libk::div_round_up(vectors, kMsixPbaBitsPerEntry) * kMsixPbaEntryBytes;
                if (bir >= kMaxBars || region >= bars[bir].size ||
                    bytes > bars[bir].size - region) {
                    return false;
                }
                if (bir == bar && start < region + bytes && region < start + size) {
                    return false;
                }
            }
        }
        offset = host[offset + 1];
    }
    return true;
}

bool config_filter_virtio_caps(GuestConfig& cfg, u32 function, const u8* host) {
    if (!host || function >= kMaxAssignFns || !cfg.present[function]) {
        return false;
    }
    // Build off to the side: malformed/cyclic host lists publish no partial chain.
    u8 filtered[kCfgSpaceBytes]{};
    bool occupied[kCfgSpaceBytes]{};
    u64 visited = 0;
    u32 previous = kRegCapPtr;
    u32 offset = (host[6] & kStatusCapabilitiesList) ? host[kRegCapPtr] : 0;
    while (offset) {
        if (offset < kCfgCapMin || offset > kCfgCapMax || (offset & 3) || occupied[offset] ||
            (visited & (1ull << (offset / 4)))) {
            return false;
        }
        visited |= 1ull << (offset / 4);
        const u32 next = host[offset + 1];
        if (host[offset] == kCapIdVendorSpecific) {
            const u32 length = host[offset + 2];
            const u32 type = host[offset + 3];
            if (length < 16 || length > kCfgSpaceBytes - offset) {
                return false;
            }
            const u32 bar = host[offset + 4];
            if (type >= 1 && type <= 4 && bar < kMaxBars && cfg.bar_mask[function][bar]) {
                if (type == 2 && length < 20) {
                    return false;
                }
                // Derive the aperture length from its sizing mask; BAR upper halves
                // are never valid targets for a virtio capability.
                const u32 raw = load(cfg.space[function], kRegBar0 + bar * 4, 4);
                if (cfg.bar_upper[function] & (1u << bar)) {
                    return false;
                }
                u64 mask = cfg.bar_mask[function][bar] & kBarAddrMask;
                if ((raw & 6) == 4 && bar + 1 < kMaxBars) {
                    mask |= static_cast<u64>(cfg.bar_mask[function][bar + 1]) << 32;
                } else {
                    mask |= kBarUpperAbsent;
                }
                const u64 size = ~mask + 1;
                const u32 start = load(host, offset + 8, 4);
                const u32 bytes = load(host, offset + 12, 4);
                if (!bytes || start >= size || bytes > size - start) {
                    return false;
                }
                const u32 exposed_length = type == 2 ? 20 : 16;
                for (u32 i = 0; i < length; ++i) {
                    // Disallow overlapping retained capabilities.
                    if (occupied[offset + i]) {
                        return false;
                    }
                    occupied[offset + i] = true;
                    if (i < exposed_length) {
                        filtered[offset + i] = host[offset + i];
                    }
                }
                filtered[offset + 2] = static_cast<u8>(exposed_length);
                filtered[previous] = static_cast<u8>(offset);
                filtered[offset + 1] = 0;
                previous = offset + 1;
            }
        }
        offset = next;
    }
    for (u32 i = kCfgCapMin; i < kCfgSpaceBytes; ++i) {
        cfg.space[function][i] = filtered[i];
    }
    cfg.space[function][kRegCapPtr] = filtered[kRegCapPtr];
    cfg.space[function][6] = filtered[kRegCapPtr] ? static_cast<u8>(kStatusCapabilitiesList) : 0;
    cfg.space[function][7] = 0;
    cfg.msi_cap_off[function] = 0;
    return true;
}
CfgWriteAction classify_config_write(const GuestConfig& cfg, u32 fn_index, u32 offset, u32 size) {
    if (fn_index >= kMaxAssignFns || !cfg.present[fn_index]) {
        return CfgWriteAction::Reject;
    }
    if ((size != 1 && size != 2 && size != 4) || (offset & (size - 1)) ||
        offset >= kCfgSpaceBytes || size > kCfgSpaceBytes - offset) {
        return CfgWriteAction::Reject;
    }
    // The command register is always mediated: the caller decides which bits a
    // guest may actually change, and the bus-master bit only after ACTIVE.
    if (offset >= kRegCommand && offset < kRegCommand + 2) {
        return CfgWriteAction::Mediate;
    }
    // BAR relocation, the expansion ROM BAR and the capability pointer must
    // never be guest-controlled: moving a BAR would point the window at another
    // function's registers or at host memory (§6.3).
    if (offset >= kRegBar0 && offset < kRegBarEnd) {
        return CfgWriteAction::Reject;
    }
    if (offset >= kRegRomBar && offset < kRegRomBar + 4) {
        return CfgWriteAction::Reject;
    }
    if (offset >= kRegCapPtr && offset < kRegCapPtr + 4) {
        return CfgWriteAction::Reject;
    }
    // Interrupt line/pin and the cache-line/latency bytes are benign fields with
    // no isolation consequences.
    if (offset == kRegIntLine && size == 1) {
        return CfgWriteAction::Accept;
    }
    if (offset >= 0x0C && offset + size <= 0x0E) {
        return CfgWriteAction::Accept;
    }
    // MSI/MSI-X capability windows are mediated: the guest may program them,
    // but the value is bent into an Anko route, never a raw host MSI target.
    const u32 msi_off = cfg.msi_cap_off[fn_index];
    if (msi_off != 0 && offset >= msi_off + kMsiCtrlOffset && offset + size <= msi_off + kMsiCapBytes) {
        return CfgWriteAction::Mediate;
    }
    // Anything else (device-specific read-only fields, power management, reset)
    // is rejected rather than blindly stored.
    return CfgWriteAction::Reject;
}

} // namespace pci
