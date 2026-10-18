#pragma once
// mochi/ipc_profile.h - on-disk form of the per-service poll budget.
//
// The file is a hint, never authority. It is parsed with hard bounds, every
// entry is checksummed, and a budget is clamped exactly like the syscall does,
// so a corrupt or hostile file can only fall back to defaults. Principal ids
// are assigned at boot and are not stable across builds, so entries are keyed
// by the service name's FNV-1a hash; a collision can only mis-tune a budget.
//
// Layout (little endian):
//   0  magic   'M','X','I','P'
//   4  version u16 = 1
//   6  count   u16 (<= kMaxEntries)
//   8  entries[count], 16 bytes each:
//        u32 name_hash, u16 budget, u16 flags, u32 check, u32 reserved
//   check = FNV-1a over name_hash, budget, flags, reserved.

#include <cstdint>
#include <cstddef>

namespace mochi {

constexpr uint32_t kIpcProfileVersion = 1;
constexpr uint32_t kIpcProfileMaxEntries = 32;
constexpr uint32_t kIpcProfileMaxBudget = 4096;   // mirrors kIpcPollBudgetMax

struct IpcProfileEntry {
    uint32_t name_hash;
    uint16_t budget;
    uint16_t flags;
    uint32_t check;
    uint32_t reserved;
};
static_assert(sizeof(IpcProfileEntry) == 16, "profile entry must stay 16 bytes");

inline uint32_t profile_fnv1a(const void* data, size_t size) {
    const auto* p = static_cast<const uint8_t*>(data);
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < size; ++i) { h ^= p[i]; h *= 16777619u; }
    return h;
}

inline uint32_t profile_name_hash(const char* name) {
    if (!name) return 0;
    size_t n = 0;
    while (name[n]) ++n;
    return profile_fnv1a(name, n);
}

// Hash exactly the 12 on-wire bytes, in wire order, so the generator and the
// parser cannot disagree about field packing.
inline uint32_t profile_check(const IpcProfileEntry& e) {
    uint8_t raw[12];
    const uint32_t name_hash = e.name_hash;
    for (unsigned i = 0; i < 4; ++i) raw[i] = static_cast<uint8_t>(name_hash >> (8 * i));
    raw[4] = static_cast<uint8_t>(e.budget);
    raw[5] = static_cast<uint8_t>(e.budget >> 8);
    raw[6] = static_cast<uint8_t>(e.flags);
    raw[7] = static_cast<uint8_t>(e.flags >> 8);
    for (unsigned i = 0; i < 4; ++i) raw[8 + i] = static_cast<uint8_t>(e.reserved >> (8 * i));
    return profile_fnv1a(raw, sizeof(raw));
}

struct IpcProfile {
    static constexpr uint32_t kMax = kIpcProfileMaxEntries;

    uint32_t count = 0;
    IpcProfileEntry entries[kMax]{};

    uint16_t lookup(uint32_t name_hash, uint16_t fallback) const {
        for (uint32_t i = 0; i < count && i < kMax; ++i)
            if (entries[i].name_hash == name_hash) return entries[i].budget;
        return fallback;
    }

    bool set(uint32_t name_hash, uint32_t budget) {
        if (budget > kIpcProfileMaxBudget) budget = kIpcProfileMaxBudget;
        for (uint32_t i = 0; i < count && i < kMax; ++i) {
            if (entries[i].name_hash != name_hash) continue;
            entries[i].budget = static_cast<uint16_t>(budget);
            entries[i].check = profile_check(entries[i]);
            return true;
        }
        if (count >= kMax) return false;
        IpcProfileEntry& e = entries[count++];
        e = IpcProfileEntry{name_hash, static_cast<uint16_t>(budget), 0, 0, 0};
        e.check = profile_check(e);
        return true;
    }

    // Strict: wrong magic/version, truncation, overlong count, bad checksum or
    // an out-of-range budget reject the whole file. Partial trust is worse than
    // no trust for a performance hint.
    bool decode(const void* data, size_t size) {
        if (!data || size < 8) return false;
        const auto* p = static_cast<const uint8_t*>(data);
        if (p[0] != 'M' || p[1] != 'X' || p[2] != 'I' || p[3] != 'P') return false;
        const uint16_t version = static_cast<uint16_t>(p[4] | p[5] << 8);
        const uint16_t entries = static_cast<uint16_t>(p[6] | p[7] << 8);
        if (version != kIpcProfileVersion || entries > kMax) return false;
        if (size < 8 + static_cast<size_t>(entries) * sizeof(IpcProfileEntry)) return false;
        IpcProfile parsed;
        for (uint16_t i = 0; i < entries; ++i) {
            IpcProfileEntry e{};
            const uint8_t* raw = p + 8 + i * sizeof(IpcProfileEntry);
            for (unsigned b = 0; b < 4; ++b) {
                e.name_hash |= static_cast<uint32_t>(raw[b]) << (8 * b);
                e.check     |= static_cast<uint32_t>(raw[8 + b]) << (8 * b);
                e.reserved  |= static_cast<uint32_t>(raw[12 + b]) << (8 * b);
            }
            e.budget = static_cast<uint16_t>(raw[4] | raw[5] << 8);
            e.flags  = static_cast<uint16_t>(raw[6] | raw[7] << 8);
            if (e.budget > kIpcProfileMaxBudget) return false;
            if (e.check != profile_check(e)) return false;
            parsed.entries[parsed.count++] = e;
        }
        *this = parsed;
        return true;
    }

    uint32_t encode(void* out, size_t capacity) const {
        const uint32_t n = count > kMax ? kMax : count;
        const size_t needed = 8 + static_cast<size_t>(n) * sizeof(IpcProfileEntry);
        if (!out || capacity < needed) return 0;
        auto* p = static_cast<uint8_t*>(out);
        p[0] = 'M'; p[1] = 'X'; p[2] = 'I'; p[3] = 'P';
        p[4] = static_cast<uint8_t>(kIpcProfileVersion);
        p[5] = static_cast<uint8_t>(kIpcProfileVersion >> 8);
        p[6] = static_cast<uint8_t>(n);
        p[7] = static_cast<uint8_t>(n >> 8);
        for (uint32_t i = 0; i < n; ++i) {
            const IpcProfileEntry& e = entries[i];
            uint8_t* raw = p + 8 + i * sizeof(IpcProfileEntry);
            const uint32_t words[4] = {e.name_hash,
                                       static_cast<uint32_t>(e.budget) | static_cast<uint32_t>(e.flags) << 16,
                                       e.check, e.reserved};
            for (unsigned w = 0; w < 4; ++w)
                for (unsigned b = 0; b < 4; ++b)
                    raw[w * 4 + b] = static_cast<uint8_t>(words[w] >> (8 * b));
        }
        return static_cast<uint32_t>(needed);
    }
};

} // namespace mochi
