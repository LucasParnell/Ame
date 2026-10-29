#include "midix/format/integrity.h"
#include "libk/hash.h"

namespace integrity {

namespace {
u32 crc_update(const Backend* backend, u32 state, libk::const_bytes data) {
    return backend && backend->crc32c_update ? backend->crc32c_update(state, data)
                                             : libk::crc32c_update(state, data);
}
} // namespace

// Progress is boot/watchdog policy, kept outside libk's pure checksum engine.
// Process at most 4 KiB between callbacks, with no callback test per byte.
u32 crc32c(const u8* data, usize size, Progress progress, const Backend* backend) {
    u32 crc = 0xffffffffu;
    for (usize offset = 0; offset < size;) {
        progress.poll();
        const usize count = size - offset < 4096 ? size - offset : 4096;
        crc = crc_update(backend, crc, libk::as_bytes(data + offset, count));
        offset += count;
    }
    return ~crc;
}

u32 crc32c_zero_field(const u8* data, usize size, usize zero_offset, usize zero_size,
                      Progress progress, const Backend* backend) {
    u32 crc = 0xffffffffu;
    if (!size)
        return ~crc;
    // Same "zeroed field" semantics, fed as three ranges so the checksum stays
    // on the wide path instead of one table lookup per byte.
    const usize z0 = zero_offset < size ? zero_offset : size;
    const usize room = size - z0;
    const usize zeros = zero_size < room ? zero_size : room;
    const usize z1 = z0 + zeros;
    auto feed = [&](const u8* bytes, usize count) {
        while (count) {
            progress.poll();
            const usize chunk = count < 4096 ? count : 4096;
            crc = crc_update(backend, crc, libk::as_bytes(bytes, chunk));
            bytes += chunk;
            count -= chunk;
        }
    };
    // Static, not automatic: image validation runs on the kernel's small
    // boot/exception stack, where a 4 KiB frame overflows into .data (it
    // zeroed g_online_cpu_mask and broke the first service spawn).
    static constexpr u8 zero_block[4096]{};
    feed(data, z0);
    for (usize done = 0; done < zeros;) {
        progress.poll();
        const usize chunk = zeros - done < sizeof(zero_block) ? zeros - done : sizeof(zero_block);
        crc = crc_update(backend, crc, libk::as_bytes(zero_block, chunk));
        done += chunk;
    }
    feed(data + z1, size - z1);
    return ~crc;
}

void sha256(const u8* data, usize size, u8 out[kSha256Size], Progress progress,
            const Backend* backend) {
    libk::Sha256 state;
    const auto compress =
        backend && backend->sha256_blocks ? backend->sha256_blocks : libk::sha256_compress;
    for (usize offset = 0; offset < size;) {
        progress.poll();
        const usize count = size - offset < 4096 ? size - offset : 4096;
        (void)state.update(libk::as_bytes(data + offset, count), compress);
        offset += count;
    }

    progress.poll();
    (void)state.finish(libk::as_bytes(out, kSha256Size), compress);
}

bool bytes_equal(const u8* a, const u8* b, usize size) {
    u8 diff = 0;
    for (usize i = 0; i < size; ++i) {
        diff |= static_cast<u8>(a[i] ^ b[i]);
    }
    return diff == 0;
}

} // namespace integrity
