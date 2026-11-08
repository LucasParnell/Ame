#include "anko/random.h"
#include "anko/fdt.h"
#include "anko/spinlock.h"
#include "libk/string.h"
#include "libk/endian.h"
#include "monocypher.h"
#if defined(ANKO_HOST_TEST) && defined(ANKO_ALLOW_WEAK_ENTROPY)
extern "C" u64 anko_random_test_uptime();
#endif
namespace security {
namespace {
alignas(16) u8 key[32];
u64 counter;
bool seeded, trusted_seed;
kernel::SpinLock lock{};
#ifndef ANKO_BOARD_HUAWEI_ANNE
bool equal(const char* a, const char* b) { return libk::strcmp(a, b) == 0; }
struct Seed { u8 bytes[32]; u8* source; usize size; bool chosen; };
bool begin(void* context, const char* name, u32 depth) {
    auto& s = *static_cast<Seed*>(context);
    if (depth == 1) s.chosen = equal(name, "chosen"); return true;
}
void end(void* context, u32 depth) { if (depth == 1) static_cast<Seed*>(context)->chosen = false; }
void property(void* context, const char* name, const u8* data, u32 bytes, u32) {
    auto& s = *static_cast<Seed*>(context);
    if (!s.chosen || !equal(name, "rng-seed") || bytes < 32 || bytes > 4096) return;
    u8 any = 0, different = 0;
    for (u32 i = 0; i < bytes; ++i) { any |= data[i]; different |= u8(data[i] ^ 0xff); }
    if (!any || !different) return;
    crypto_blake2b(s.bytes, sizeof(s.bytes), data, bytes);
    s.source = const_cast<u8*>(data); s.size = bytes;
}
#endif
#ifdef ANKO_BOARD_HUAWEI_ANNE
struct Smc { u64 status, a, b, c; };
Smc smc(u64 function, u64 argument) {
    register u64 x0 asm("x0") = function, x1 asm("x1") = argument;
    register u64 x2 asm("x2") = 0, x3 asm("x3") = 0;
    asm volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) ::
        "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
    return {x0, x1, x2, x3};
}
bool hardware_seed(u8* out) {
    // Arm's TRNG firmware ABI keeps status separate from entropy; never treat
    // an unsupported SMC or a firmware error code as a random word.
    auto version = smc(0x84000050, 0);
    if (i64(version.status) < 0 || (version.status >> 16) != 1) return false;
    u64 previous = 0; bool have_previous = false;
    for (usize offset = 0; offset < 32; offset += 8) {
        bool received = false;
        for (unsigned retry = 0; retry < 128; ++retry) {
            auto result = smc(0xc4000053, 64);
            if (i64(result.status) == -3) continue; // NO_ENTROPY, bounded retry
            if (result.status) return false;
            const u64 word = result.c;
            if (!word || word == ~u64{0} || (have_previous && word == previous)) return false;
            libk::memcpy(out + offset, &word, 8); previous = word; have_previous = true;
            received = true; break;
        }
        if (!received) return false;
    }
    return true;
}
#endif
}
bool seed_boot_random(PhysAddr dtb) {
    if (seeded) return true;
    struct { u8 bytes[32]; u8* source; usize size; } seed{}; bool trusted = false;
#ifdef ANKO_BOARD_HUAWEI_ANNE
    // Prefer the hardware/firmware source before the temporary fallback.
    (void)dtb; trusted = hardware_seed(seed.bytes);
#else
    Seed collected{};
    if (dtb) trusted = fdt::walk(reinterpret_cast<const u8*>(dtb), {&collected, begin, end, property}) && collected.source;
    libk::memcpy(seed.bytes, collected.bytes, 32); seed.source = collected.source; seed.size = collected.size;
    crypto_wipe(&collected, sizeof(collected));
#endif
    if (!trusted) {
#ifdef ANKO_ALLOW_WEAK_ENTROPY
        // Explicit bring-up fallback requested for hardware without an audited
        // HRNG path. Counter uptime is predictable; hashing does not promote
        // this input to trusted entropy. Keep the source quality observable.
        for (usize offset = 0; offset < sizeof(seed.bytes); offset += sizeof(u64)) {
            u64 uptime;
#if defined(ANKO_HOST_TEST)
            uptime = anko_random_test_uptime();
#else
            asm volatile("mrs %0, cntpct_el0" : "=r"(uptime));
#endif
            libk::memcpy(seed.bytes + offset, &uptime, sizeof(uptime));
        }
#else
        crypto_wipe(&seed, sizeof(seed)); return false;
#endif
    }
    trusted_seed = trusted;
    crypto_blake2b(key, sizeof(key), seed.bytes, sizeof(seed.bytes));
    if (seed.source) crypto_wipe(seed.source, seed.size);
    crypto_wipe(&seed, sizeof(seed)); counter = 1; seeded = true; return true;
}
bool random_ready() { return seeded; }
bool random_trusted() { return seeded && trusted_seed; }
bool random_bytes(void* output, usize size) {
    if ((!output && size) || !seeded) return false;
    kernel::LockGuard guard(lock);
    auto* bytes = static_cast<u8*>(output);
    while (size) {
        u8 block[64], nonce[8]{};
        // Rekey on every block for backtracking resistance; no output exposes
        // the next generator state. The pool never leaves supervisor memory.
        crypto_chacha20_djb(block, nullptr, sizeof(block), key, nonce, counter++);
        libk::memcpy(key, block, sizeof(key));
        const usize take = size < 32 ? size : 32;
        libk::memcpy(bytes, block + 32, take); bytes += take; size -= take;
        crypto_wipe(block, sizeof(block));
    }
    return true;
}
u64 random_below(u64 bound) {
    if (bound < 2 || !seeded) return 0;
    const u64 threshold = -bound % bound;
    u64 sample;
    do { if (!random_bytes(&sample, sizeof(sample))) return 0; } while (sample < threshold);
    return sample % bound;
}
}
