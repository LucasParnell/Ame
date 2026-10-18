#include "../userland/fs/store.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <memory>
#include <stdexcept>
#include <unistd.h>
using namespace mochi::store;
using Block = std::array<u8, kBlock>;
struct PowerCut {};
struct Disk {
    int fd;
    std::map<u64, Block> stable, volatile_pages;
    int cut = -1, events = 0;
    bool reverse = false;
    explicit Disk(int file) : fd(file) {}
    void tick() {
        if (events++ == cut)
            throw PowerCut{};
    }
    static bool read(void *context, u64 first, u32 count, void *output) {
        auto &d = *static_cast<Disk *>(context);
        for (u32 i = 0; i < count; ++i) {
            auto *out = static_cast<u8 *>(output) + i * kBlock;
            auto v = d.volatile_pages.find(first + i), p = d.stable.find(first + i);
            if (v != d.volatile_pages.end())
                std::memcpy(out, v->second.data(), kBlock);
            else if (p != d.stable.end())
                std::memcpy(out, p->second.data(), kBlock);
            else
                assert(pread(d.fd, out, kBlock, (first + i) * kBlock) == kBlock);
        }
        return true;
    }
    static bool write(void *context, u64 first, u32 count, const void *input) {
        auto &d = *static_cast<Disk *>(context);
        for (u32 i = 0; i < count; ++i) {
            auto &out = d.volatile_pages[first + i];
            std::memcpy(out.data(), static_cast<const u8 *>(input) + i * kBlock, kBlock);
            d.tick();
        }
        return true;
    }
    static bool flush(void *context) {
        auto &d = *static_cast<Disk *>(context);
        auto persist = [&](u64 block, const Block &value) {
            Block original{};
            read(&d, block, 1, original.data());
            // Read the stable version rather than the unflushed cache.
            auto found = d.stable.find(block);
            if (found != d.stable.end())
                original = found->second;
            else
                assert(pread(d.fd, original.data(), kBlock, block * kBlock) == kBlock);
            auto &out = d.stable[block];
            out = original;
            for (u32 sector = 0; sector < 8; ++sector) {
                std::memcpy(out.data() + sector * 512, value.data() + sector * 512, 512);
                d.tick();
            }
        };
        if (d.reverse)
            for (auto it = d.volatile_pages.rbegin(); it != d.volatile_pages.rend(); ++it)
                persist(it->first, it->second);
        else
            for (const auto &[block, value] : d.volatile_pages)
                persist(block, value);
        d.volatile_pages.clear();
        d.tick();
        return true;
    }
    Io io() { return {this, read, write, flush}; }
    void reboot() {
        volatile_pages.clear();
        cut = -1;
        events = 0;
    }
};
static InodeLeafEntry child(State &s, u32 parent, const char *name, u16 kind = FS_INODE_FILE) {
    InodeLeafEntry i{};
    auto status = create(s, parent, name, u8(std::strlen(name)), kind, i);
    if (status != MX_OK)
        std::fprintf(stderr, "create %s status=%d error=%d pages=%u\n", name, status, s.error, s.page_count);
    assert(status == MX_OK);
    return i;
}
static void verify(State &s, u32 ino, u64 offset, const void *expected, u32 length) {
    std::array<u8, 65536> result{};
    u32 n = 0;
    assert(length <= result.size());
    auto status = read(s, ino, offset, result.data(), length, n);
    if (status)
        std::fprintf(stderr, "read status=%d error=%d\n", status, s.error);
    assert(status == MX_OK && n == length && !std::memcmp(result.data(), expected, length));
}
template <class Mutate, class Verify>
static void crash_matrix(Disk seed, State &state, const char *operation, Mutate mutate, Verify verify_state) {
    for (bool reverse : {false, true}) {
        Disk complete = seed;
        complete.reverse = reverse;
        assert(mount(state, complete.io()));
        complete.events = 0;
        assert(mutate(state) == MX_OK);
        const auto events = complete.events;
        for (int cut = 0; cut < events; ++cut) {
            Disk torn = seed;
            torn.reverse = reverse;
            if (!mount(state, torn.io())) {
                std::fprintf(stderr,"recovery failed operation=%s cut=%d reverse=%d error=%d orphans=%u\n",operation,cut,reverse,state.error,state.orphans);
                assert(false);
            }
            torn.events = 0;
            torn.cut = cut;
            try {
                (void)mutate(state);
            } catch (PowerCut &) {
            }
            torn.reboot();
            if (!mount(state, torn.io())) {
                std::fprintf(stderr,"recovery failed operation=%s cut=%d reverse=%d error=%d orphans=%u\n",operation,cut,reverse,state.error,state.orphans);
                assert(false);
            }
            assert(!state.orphans && !state.poisoned);
            verify_state(state);
        }
        std::printf("%s power-cut points: %d reverse=%d\n", operation, events, reverse);
    }
}
int main(int argc, char **argv) {
    assert(argc == 2 || argc == 3);
    int fd = open(argv[1], O_RDONLY);
    assert(fd >= 0);
    Disk disk(fd);
    auto s = std::make_unique<State>();
    assert(mount(*s, disk.io()));
    assert(s->super.total_blocks > 256);
    if (argc == 3) {
        auto file = child(*s, kDataRoot, "full");
        std::array<u8, 65536> bytes{};
        bytes.fill(0x9b);
        u64 offset = 0;
        for (;;) {
            const auto result = write(*s, file.inode_number, offset, bytes.data(), bytes.size());
            if (result != MX_OK) {
                assert(result == MX_ERR_NOMEM && !s->poisoned);
                break;
            }
            offset += bytes.size();
            assert(offset <= s->super.total_blocks * kBlock);
        }
        assert(offset > 1024 * 1024);
        disk.reboot();
        assert(mount(*s, disk.io()));
        verify(*s, file.inode_number, offset - bytes.size(), bytes.data(), bytes.size());
        assert(truncate(*s, file.inode_number) == MX_OK);
        assert(s->reserve_count == kReserve && !s->orphans);
        for (u32 i = 0; i < 4; ++i)
            assert(write(*s, file.inode_number, i * bytes.size(), bytes.data(), bytes.size()) == MX_OK);
        disk.reboot();
        assert(mount(*s, disk.io()));
        verify(*s, file.inode_number, 3 * bytes.size(), bytes.data(), bytes.size());
        assert(unlink(*s, kDataRoot, "full", 4) == MX_OK);
        close(fd);
        std::puts("ENOSPC: committed bytes intact; truncate/unlink reclaim at capacity passed");
        return 0;
    }
    auto dir = child(*s, kDataRoot, "many", FS_INODE_DIR);
    for (unsigned i = 0; i < 300; ++i) {
        char name[20];
        std::snprintf(name, sizeof(name), "f-%05u", i);
        child(*s, dir.inode_number, name);
    }
    for (unsigned i = 0; i < 300; ++i) {
        char name[20];
        std::snprintf(name, sizeof(name), "f-%05u", i);
        InodeLeafEntry file{};
        assert(lookup(*s, dir.inode_number, name, u8(std::strlen(name)), file) == MX_OK);
    }
    for (unsigned i = 0; i < 300; ++i) {
        DirLeafEntry e{};
        bool eof = false;
        assert(readdir(*s, dir.inode_number, i, e, eof) == MX_OK && !eof);
    }
    auto file = child(*s, kDataRoot, "large");
    std::array<u8, 65536> bytes{};
    for (u32 i = 0; i < bytes.size(); ++i)
        bytes[i] = u8(i * 17 + 3);
    for (u32 i = 0; i < 40; ++i) {
        auto status = write(*s, file.inode_number, u64(i) * bytes.size(), bytes.data(), bytes.size());
        if (status)
            std::fprintf(stderr, "write chunk=%u error=%d\n", i, status);
        assert(status == MX_OK);
    }
    verify(*s, file.inode_number, 39 * bytes.size(), bytes.data(), bytes.size());
    // Force allocation at the physical end, across the bitmap range boundary.
    s->allocation_hint = s->super.total_blocks - 20;
    assert(write(*s, file.inode_number, 40 * bytes.size(), bytes.data(), bytes.size()) == MX_OK);
    bool physical_tail = false;
    for (const auto &[block, value] : disk.stable)
        if (block >= s->super.total_blocks - 20 && value[0] == bytes[0])
            physical_tail = true;
    assert(physical_tail);
    InodeLeafEntry clone{};
    assert(promote(*s, file.inode_number, clone) == MX_OK);
    const u8 change = 0x7a;
    assert(write(*s, file.inode_number, 0, &change, 1) == MX_OK);
    verify(*s, clone.inode_number, 0, bytes.data(), bytes.size());
    assert(write(*s, file.inode_number, 0, bytes.data(), bytes.size()) == MX_OK);
    // Reclaiming the source must preserve every block shared with the clone.
    constexpr u64 high = 50ull * 1024 * 1024 * 1024;
    const u8 marker = 0xbc;
    assert(write(*s, file.inode_number, high, &marker, 1) == MX_OK);
    verify(*s, file.inode_number, high, &marker, 1);
    assert(sync(*s) == MX_OK);
    disk.reboot();
    assert(mount(*s, disk.io()));
    verify(*s, file.inode_number, high, &marker, 1);
    verify(*s, file.inode_number, 0, bytes.data(), bytes.size());
    std::array<u8, 64> zero{};
    verify(*s, file.inode_number, high - 64, zero.data(), 64);
    assert(rename(*s, kDataRoot, "large", 5, dir.inode_number, "renamed", 7) == MX_OK);
    assert(rename(*s, kDataRoot, "many", 4, dir.inode_number, "cycle", 5) == MX_ERR_INVALID);
    assert(truncate(*s, file.inode_number) == MX_OK);
    verify(*s, clone.inode_number, 40 * bytes.size(), bytes.data(), bytes.size());
    assert(unlink(*s, dir.inode_number, "renamed", 7) == MX_OK);
    // Recover complete metadata transactions under reordered, sector-torn writes.
    Disk seed(fd);
    assert(mount(*s, seed.io()));
    auto probe = child(*s, kDataRoot, "crash");
    const char old[] = "previous durable contents", replacement[] = "replacement durable bytes";
    static_assert(sizeof(old) == sizeof(replacement));
    assert(write(*s, probe.inode_number, 0, old, sizeof(old)) == MX_OK);
    assert(sync(*s) == MX_OK);
    for (bool reverse : {false, true}) {
        Disk complete = seed;
        complete.reverse = reverse;
        complete.events = 0;
        assert(mount(*s, complete.io()));
        complete.events = 0;
        assert(write(*s, probe.inode_number, 0, replacement, sizeof(replacement)) == MX_OK);
        const auto events = complete.events;
        for (int cut = 0; cut < events; ++cut) {
            Disk torn = seed;
            torn.reverse = reverse;
            assert(mount(*s, torn.io()));
            torn.events = 0;
            torn.cut = cut;
            try {
                (void)write(*s, probe.inode_number, 0, replacement, sizeof(replacement));
            } catch (PowerCut &) {
            }
            torn.reboot();
            assert(mount(*s, torn.io()));
            std::array<u8, sizeof(old)> actual{};
            u32 n = 0;
            assert(read(*s, probe.inode_number, 0, actual.data(), actual.size(), n) == MX_OK &&
                   n == sizeof(old));
            assert(!std::memcmp(actual.data(), old, sizeof(old)) ||
                   !std::memcmp(actual.data(), replacement, sizeof(replacement)));
        }
        std::printf("power cut points verified: %d reverse=%d\n", events, reverse);
    }
    Disk metadata_seed = seed;
    assert(mount(*s, metadata_seed.io()));
    for (u32 i = 0; i < 3; ++i)
        assert(write(*s, probe.inode_number, i * bytes.size(), bytes.data(), bytes.size()) == MX_OK);
    crash_matrix(
        metadata_seed, *s, "truncate", [&](State &st) { return truncate(st, probe.inode_number); },
        [&](State &st) {
            InodeLeafEntry current{};
            assert(inode(st, probe.inode_number, current));
            assert(current.size == 0 || current.size == 3 * bytes.size());
            if (current.size)
                verify(st, probe.inode_number, 2 * bytes.size(), bytes.data(), bytes.size());
        });
    crash_matrix(
        metadata_seed, *s, "unlink", [&](State &st) { return unlink(st, kDataRoot, "crash", 5); },
        [&](State &st) {
            InodeLeafEntry current{};
            auto result = lookup(st, kDataRoot, "crash", 5, current);
            assert(result == MX_OK || result == MX_ERR_NOT_FOUND);
            if (result == MX_OK)
                verify(st, current.inode_number, 2 * bytes.size(), bytes.data(), bytes.size());
        });
    crash_matrix(
        metadata_seed, *s, "rename",
        [&](State &st) { return rename(st, kDataRoot, "crash", 5, kDataRoot, "renamed", 7); },
        [&](State &st) {
            InodeLeafEntry old_name{}, new_name{};
            const auto old_result = lookup(st, kDataRoot, "crash", 5, old_name),
                       new_result = lookup(st, kDataRoot, "renamed", 7, new_name);
            assert((old_result == MX_OK && new_result == MX_ERR_NOT_FOUND) ||
                   (old_result == MX_ERR_NOT_FOUND && new_result == MX_OK));
            verify(st, probe.inode_number, 2 * bytes.size(), bytes.data(), bytes.size());
        });
    crash_matrix(
        metadata_seed, *s, "promote",
        [&](State &st) {
            InodeLeafEntry promoted{};
            return promote(st, probe.inode_number, promoted);
        },
        [&](State &st) {
            for (u32 ordinal = 0;; ++ordinal) {
                DirLeafEntry row{};
                bool eof = false;
                assert(readdir(st, kDataRoot, ordinal, row, eof) == MX_OK);
                if (eof)
                    break;
                verify(st, row.child_inode, 2 * bytes.size(), bytes.data(), bytes.size());
            }
        });
    close(fd);
    std::puts("disk store: full-volume allocation, scalable B+trees, sparse I/O, rename, truncate, recovery "
              "passed");
}
