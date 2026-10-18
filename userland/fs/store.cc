#include "store.h"
#include "libk/endian.h"
#include "libk/hash.h"
#include "libk/str.h"

namespace mochi::store {
namespace {
constexpr u32 kCoverage = kBlock * 4, kRecordType = JRN_METADATA_FRAGMENT, kFragmentBytes = 4056;
mx_i32_t reap(State &, u32);
constexpr u32 kCounts = 1, kExtension = 0x31585446; // FTX1 checkpoint extension
struct Record {
    u64 lsn;
    u32 type, target;
    u64 timestamp;
    u32 crc, reserved;
    u8 data[4064];
};
struct Commit {
    u64 lsn, first;
    u32 count, crc;
    u64 timestamp;
    u8 pad[4064];
};
static_assert(sizeof(Record) == kBlock && sizeof(Commit) == kBlock);
void copy(void *to, const void *from, u32 n) {
    libk::copy_exact(libk::as_bytes(to, n), libk::as_bytes(from, n));
}
u32 crc(const void *p, u32 n) { return libk::crc32_ieee(libk::as_bytes(p, n)); }
bool fail(State &s, mx_i32_t error = MX_ERR_BAD_IMAGE) {
    if (!s.error)
        s.error = error;
    return false;
}
bool get(State &s, u64 block, void *out) {
    if (block >= s.super.total_blocks)
        return fail(s);
    for (u32 i = 0; i < s.page_count; ++i)
        if (s.page_ids[i] == block) {
            copy(out, s.pages[i], kBlock);
            return true;
        }
    if (!s.io.read(s.io.context, block, 1, out))
        return fail(s, MX_ERR_FAULT);
    return true;
}
u8 *stage(State &s, u64 block, bool blank = false) {
    for (u32 i = 0; i < s.page_count; ++i)
        if (s.page_ids[i] == block)
            return s.pages[i];
    if (block < s.super.first_data_block || block >= s.super.total_blocks) {
        fail(s);
        return nullptr;
    }
    if (s.page_count == kPages) {
        fail(s, MX_ERR_NOMEM);
        return nullptr;
    }
    auto *page = s.pages[s.page_count];
    if (blank)
        libk::zero(page, kBlock);
    else if (!get(s, block, page))
        return nullptr;
    s.page_ids[s.page_count++] = block;
    return page;
}
BTreeNodeHeader &header(u8 *p) { return *reinterpret_cast<BTreeNodeHeader *>(p); }
u8 *entry(u8 *p, u32 i) { return p + 64 + i * 64; }
u64 key(const u8 *p, bool inodes) { return inodes ? libk::load_le<u32>(p) : libk::load_le<u64>(p); }
u32 bound(u8 *p, u64 target, bool inodes, bool upper = false) {
    auto &h = header(p);
    u32 lo = 0, hi = h.entry_count;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        auto k = key(entry(p, mid), h.node_type == 2 && inodes);
        if (k < target || (upper && k == target))
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}
bool valid_node(State &s, u8 *p, bool inodes) {
    const auto &h = header(p);
    if ((h.node_type != 1 && h.node_type != 2) || h.entry_count > kEntries)
        return fail(s);
    for (u32 i = 1; i < h.entry_count; ++i)
        if (key(entry(p, i - 1), h.node_type == 2 && inodes) > key(entry(p, i), h.node_type == 2 && inodes))
            return fail(s);
    return true;
}
// Exact lookup or predecessor; no pointers into scratch survive another read.
bool find(State &s, u64 root, u64 target, bool inodes, void *out, bool floor = false) {
    if (!root)
        return false;
    u64 previous = 0;
    u32 previous_depth = 0;
    bool rightmost = false;
    for (u32 depth = 0; depth < kDepth; ++depth) {
        if (!get(s, root, s.lookup) || !valid_node(s, s.lookup, inodes))
            return false;
        auto &h = header(s.lookup);
        u32 i = rightmost ? h.entry_count : bound(s.lookup, target, inodes, floor);
        if (h.node_type == 2) {
            if (floor) {
                if (!i) {
                    if (!previous)
                        return false;
                    root = previous;
                    depth = previous_depth - 1;
                    previous = 0;
                    rightmost = true;
                    continue;
                }
                --i;
            }
            if (i == h.entry_count || (!floor && key(entry(s.lookup, i), inodes) != target))
                return false;
            copy(out, entry(s.lookup, i), 64);
            return true;
        }
        // Internal separators are inclusive maxima. Floor still follows the
        // first maximum >= target; the preceding subtree is used at leaf gap.
        i = rightmost ? h.entry_count : bound(s.lookup, target, false);
        if (i == h.entry_count) {
            if (!floor || !i)
                return false;
            --i;
        }
        if (floor && !rightmost && i) {
            previous = libk::load_le<u64>(entry(s.lookup, i - 1) + 8);
            previous_depth = depth + 1;
        }
        root = libk::load_le<u64>(entry(s.lookup, i) + 8);
        if (!root)
            return fail(s);
    }
    return fail(s);
}
// Rank lookup uses persisted child counts on new internal nodes. Legacy image
// nodes are counted recursively once as they are rewritten.
u64 count(State &s, u64 root, u32 depth = 0, bool inodes = false) {
    if (!root)
        return 0;
    if (depth == kDepth || !get(s, root, s.path[depth]) || !valid_node(s, s.path[depth], inodes)) {
        fail(s);
        return 0;
    }
    auto *p = s.path[depth];
    auto h = header(p);
    if (h.node_type == 2)
        return h.entry_count;
    u64 n = 0;
    for (u32 i = 0; i < h.entry_count; ++i) {
        u64 child = libk::load_le<u64>(entry(p, i) + 8);
        u64 amount =
            (h.flags & kCounts) ? libk::load_le<u64>(entry(p, i) + 16) : count(s, child, depth + 1, inodes);
        if (~u64{0} - n < amount) {
            fail(s);
            return 0;
        }
        n += amount;
    }
    return n;
}
bool nth(State &s, u64 root, u64 rank, bool inodes, void *out, u32 depth = 0) {
    if (!root)
        return false;
    if (depth == kDepth || !get(s, root, s.path[depth]) || !valid_node(s, s.path[depth], inodes))
        return fail(s);
    auto *p = s.path[depth];
    auto h = header(p);
    if (h.node_type == 2) {
        if (rank >= h.entry_count)
            return false;
        copy(out, entry(p, u32(rank)), 64);
        return true;
    }
    for (u32 i = 0; i < h.entry_count; ++i) {
        u64 child = libk::load_le<u64>(entry(p, i) + 8);
        u64 amount =
            (h.flags & kCounts) ? libk::load_le<u64>(entry(p, i) + 16) : count(s, child, depth + 1, inodes);
        if (s.error)
            return false;
        if (rank < amount)
            return nth(s, child, rank, inodes, out, depth + 1);
        rank -= amount;
    }
    return false;
}
bool bitmap(State &s, u64 block, u8 *&page, u32 &shift, u32 &offset) {
    FreeSpaceLeafEntry free{};
    if (!find(s, s.pending.extent_bitmap_root, (block / kCoverage) * kCoverage, false, &free) ||
        block < free.range_start || block >= free.range_end || free.range_end - free.range_start > kCoverage)
        return fail(s);
    page = stage(s, free.bitmap_block);
    if (!page)
        return false;
    auto relative = block - free.range_start;
    offset = u32(relative / 4);
    shift = u32(relative % 4) * 2;
    return true;
}
u64 allocate(State &s) {
    u64 start = s.allocation_hint;
    if (start < s.super.first_data_block || start >= s.super.total_blocks)
        start = s.super.first_data_block;
    for (u32 pass = 0; pass < 2; ++pass) {
        u64 b = pass ? s.super.first_data_block : start;
        const u64 limit = pass ? start : s.super.total_blocks;
        while (b < limit) {
            FreeSpaceLeafEntry free{};
            if (!find(s, s.pending.extent_bitmap_root, (b / kCoverage) * kCoverage, false, &free) ||
                b < free.range_start || b >= free.range_end ||
                free.range_end - free.range_start > kCoverage) {
                fail(s);
                return 0;
            }
            if (!get(s, free.bitmap_block, s.lookup))
                return 0;
            const u64 end = free.range_end < limit ? free.range_end : limit;
            for (; b < end; ++b) {
                u32 off = u32((b - free.range_start) / 4), sh = u32((b - free.range_start) % 4) * 2;
                if (!sh && s.lookup[off] == 255 && end - b >= 4) {
                    b += 3;
                    continue;
                }
                if ((s.lookup[off] >> sh) & 3)
                    continue;
                auto *page = stage(s, free.bitmap_block);
                if (!page)
                    return 0;
                page[off] |= u8(1u << sh);
                s.allocation_hint = b + 1;
                return b;
            }
        }
    }
    if (s.reclaim && s.reserve_count) {
        const auto block = s.reserve[--s.reserve_count];
        u8 *page = nullptr;
        u32 shift = 0, offset = 0;
        if (!bitmap(s, block, page, shift, offset))
            return 0;
        if (((page[offset] >> shift) & 3) != 3) {
            fail(s);
            return 0;
        }
        page[offset] = u8((page[offset] & ~(3u << shift)) | (1u << shift));
        return block;
    }
    fail(s, MX_ERR_NOMEM);
    return 0;
}
bool release(State &s, u64 block) {
    if (block < s.super.first_data_block || block >= s.super.total_blocks)
        return fail(s);
    u8 *p = nullptr;
    u32 sh = 0, off = 0;
    if (!bitmap(s, block, p, sh, off))
        return false;
    if (((p[off] >> sh) & 3) != 1)
        return fail(s);
    if (s.free_count == kFrees)
        return fail(s, MX_ERR_NOMEM);
    p[off] = u8((p[off] & ~(3u << sh)) | (2u << sh));
    s.freed[s.free_count++] = block;
    // A staged page must not later be written over a reused data block.
    for (u32 i = 0; i < s.page_count; ++i)
        if (s.page_ids[i] == block) {
            --s.page_count;
            if (i != s.page_count) {
                s.page_ids[i] = s.page_ids[s.page_count];
                copy(s.pages[i], s.pages[s.page_count], kBlock);
            }
            break;
        }
    return true;
}
bool put_page(State &s, u64 block, const u8 *data) {
    auto *out = stage(s, block, true);
    if (!out)
        return false;
    copy(out, data, kBlock);
    return true;
}
struct Branch {
    u64 block{}, maximum{}, amount{}, right{}, right_max{}, right_amount{};
};
void separator(u8 *p, u64 maximum, u64 block, u64 amount) {
    libk::zero(p, 64);
    copy(p, &maximum, 8);
    copy(p + 8, &block, 8);
    copy(p + 16, &amount, 8);
}
Branch mutate(State &s, u64 block, u64 target, const u8 *value, bool inodes, bool erase, u32 depth,
              u32 mode) {
    Branch result{};
    if (depth == kDepth) {
        fail(s, MX_ERR_OVERFLOW);
        return result;
    }
    auto *p = s.path[depth];
    if (block) {
        if (!get(s, block, p) || !valid_node(s, p, inodes))
            return result;
    } else {
        if (erase)
            return result;
        libk::zero(p, kBlock + 64);
        header(p).node_type = 2;
        block = allocate(s);
        if (!block)
            return result;
    }
    auto &h = header(p);
    u32 position = mode == 2 ? u32(target) : bound(p, target, inodes);
    if (h.node_type == 2) {
        const bool found =
            position < h.entry_count && (mode == 2 || key(entry(p, position), inodes) == target);
        if (erase) {
            if (!found) {
                fail(s, MX_ERR_NOT_FOUND);
                return result;
            }
            for (u32 i = position + 1; i < h.entry_count; ++i)
                copy(entry(p, i - 1), entry(p, i), 64);
            --h.entry_count;
        } else if (found && mode != 1)
            copy(entry(p, position), value, 64);
        else {
            for (u32 i = h.entry_count; i > position; --i)
                copy(entry(p, i), entry(p, i - 1), 64);
            copy(entry(p, position), value, 64);
            ++h.entry_count;
        }
    } else {
        // Populate legacy child weights before descending. Subsequent nth
        // queries skip whole subtrees instead of rescanning every preceding row.
        if (!(h.flags & kCounts)) {
            for (u32 i = 0; i < h.entry_count; ++i) {
                auto amount = count(s, libk::load_le<u64>(entry(p, i) + 8), depth + 1, inodes);
                copy(entry(p, i) + 16, &amount, 8);
            }
            h.flags |= kCounts;
        }
        if (mode == 2) {
            position = 0;
            while (position < h.entry_count) {
                const auto amount = libk::load_le<u64>(entry(p, position) + 16);
                if (target < amount)
                    break;
                target -= amount;
                ++position;
            }
        }
        if (position >= h.entry_count) {
            if (erase || !h.entry_count) {
                fail(s, MX_ERR_NOT_FOUND);
                return result;
            }
            position = h.entry_count - 1;
        }
        auto child = mutate(s, libk::load_le<u64>(entry(p, position) + 8), target, value, inodes, erase,
                            depth + 1, mode);
        if (s.error)
            return result;
        if (!child.block) {
            for (u32 i = position + 1; i < h.entry_count; ++i)
                copy(entry(p, i - 1), entry(p, i), 64);
            --h.entry_count;
        } else {
            separator(entry(p, position), child.maximum, child.block, child.amount);
            if (child.right) {
                for (u32 i = h.entry_count; i > position + 1; --i)
                    copy(entry(p, i), entry(p, i - 1), 64);
                separator(entry(p, position + 1), child.right_max, child.right, child.right_amount);
                ++h.entry_count;
            }
        }
    }
    if (s.error)
        return result;
    if (!h.entry_count) {
        release(s, block);
        return result;
    }
    h.generation = s.pending.current_generation;
    h.parent_block = 0;
    h.next_sibling = 0;
    auto amount = [&](u32 first, u32 end) {
        u64 n = 0;
        for (u32 i = first; i < end; ++i)
            n += h.node_type == 2 ? 1 : libk::load_le<u64>(entry(p, i) + 16);
        return n;
    };
    result.block = block;
    if (h.entry_count > kEntries) {
        result.right = allocate(s);
        if (!result.right)
            return {};
        auto *right = stage(s, result.right, true);
        if (!right)
            return {};
        copy(right, p, 64);
        header(right).entry_count = h.entry_count / 2;
        const u32 left = h.entry_count - header(right).entry_count;
        copy(entry(right, 0), entry(p, left), header(right).entry_count * 64);
        result.right_max = key(entry(p, h.entry_count - 1), h.node_type == 2 && inodes);
        result.right_amount = amount(left, h.entry_count);
        h.entry_count = u16(left);
    }
    result.maximum = key(entry(p, h.entry_count - 1), h.node_type == 2 && inodes);
    result.amount = amount(0, h.entry_count);
    if (!put_page(s, block, p))
        return {};
    return result;
}
bool update(State &s, u64 &root, u64 target, const void *value, bool inodes = false, bool erase = false,
            u32 mode = 0) {
    auto result = mutate(s, root, target, static_cast<const u8 *>(value), inodes, erase, 0, mode);
    if (s.error)
        return false;
    root = result.block;
    if (result.right) {
        const auto block = allocate(s);
        if (!block)
            return false;
        auto *p = stage(s, block, true);
        if (!p)
            return false;
        header(p) = {1, 2, kCounts, s.pending.current_generation, 0, 0, {}};
        separator(entry(p, 0), result.maximum, result.block, result.amount);
        separator(entry(p, 1), result.right_max, result.right, result.right_amount);
        root = block;
    } else if (root) {
        if (!get(s, root, s.lookup))
            return false;
        if (header(s.lookup).node_type == 1 && header(s.lookup).entry_count == 1) {
            auto old = root;
            root = libk::load_le<u64>(entry(s.lookup, 0) + 8);
            if (!release(s, old))
                return false;
        }
    }
    return true;
}
bool begin(State &s) {
    s.error = 0;
    if (!s.mounted)
        return fail(s, MX_ERR_SERVICE_UNAVAILABLE);
    if (s.poisoned)
        return fail(s, MX_ERR_FAULT);
    if (s.transaction)
        return fail(s, MX_ERR_BUSY);
    s.pending = s.checkpoint;
    s.page_count = 0;
    s.free_count = 0;
    s.transaction = true;
    s.reclaim = false;
    if (s.pending.current_generation == ~u64{0})
        return fail(s, MX_ERR_OVERFLOW);
    ++s.pending.current_generation;
    return true;
}
bool raw_write(State &s, u64 block, u32 n, const void *p) {
    if (!n || block >= s.super.total_blocks || n > s.super.total_blocks - block ||
        !s.io.write(s.io.context, block, n, p)) {
        s.poisoned = true;
        return fail(s, MX_ERR_FAULT);
    }
    return true;
}
bool flush(State &s) {
    if (!s.io.flush(s.io.context)) {
        s.poisoned = true;
        return fail(s, MX_ERR_FAULT);
    }
    return true;
}
void checkpoint_crc(State &s) {
    auto &cp = s.pending;
    cp.crc32 = crc(&cp, 56);
    libk::zero(cp.padding, sizeof(cp.padding));
    copy(cp.padding, &kExtension, 4);
    copy(cp.padding + 4, &s.next_inode, 4);
    copy(cp.padding + 8, &s.orphans, 4);
    copy(cp.padding + 12, &s.reserve_count, 4);
    copy(cp.padding + 16, s.reserve, sizeof(s.reserve));
    const u32 check = crc(&cp, 64 + 16 + sizeof(s.reserve));
    copy(cp.padding + 16 + sizeof(s.reserve), &check, 4);
}
bool valid_checkpoint(const FsCheckpoint &cp) {
    if (crc(&cp, 56) != cp.crc32)
        return false;
    return libk::load_le<u32>(cp.padding) != kExtension ||
           (libk::load_le<u32>(cp.padding + 12) <= kReserve &&
            crc(&cp, 64 + 16 + 8 * kReserve) == libk::load_le<u32>(cp.padding + 16 + 8 * kReserve));
}
void restore_extra(State &s) {
    s.orphans = s.reserve_count = 0;
    if (libk::load_le<u32>(s.checkpoint.padding) == kExtension) {
        s.orphans = libk::load_le<u32>(s.checkpoint.padding + 8);
        s.reserve_count = libk::load_le<u32>(s.checkpoint.padding + 12);
        copy(s.reserve, s.checkpoint.padding + 16, sizeof(s.reserve));
    }
}
bool commit(State &s) {
    if (s.error) {
        s.page_count = 0;
        s.transaction = false;
        return false;
    }
    for (u32 i = 0; i < s.free_count; ++i) {
        u8 *page = nullptr;
        u32 sh = 0, off = 0;
        if (!bitmap(s, s.freed[i], page, sh, off)) {
            s.page_count = 0;
            s.transaction = false;
            return false;
        }
        page[off] &= u8(~(3u << sh));
        if (s.reserve_count < kReserve) {
            page[off] |= u8(3u << sh);
            s.reserve[s.reserve_count++] = s.freed[i];
        } else if (s.freed[i] < s.allocation_hint)
            s.allocation_hint = s.freed[i];
    }
    const u32 records = (s.page_count + 1) * 2;
    if (records + 1 > s.super.journal_block_count || s.checkpoint.lsn > ~u64{0} - records - 1) {
        fail(s, MX_ERR_OVERFLOW);
        s.page_count = 0;
        s.transaction = false;
        return false;
    }
    s.pending.lsn = s.checkpoint.lsn + records + 1;
    checkpoint_crc(s);
    // Two fragment records per 4 KiB metadata block. The target, offset and
    // length are repeated inside CRC-covered data; damaged headers cannot
    // redirect a valid record onto an unrelated block.
    u32 batch_count = 0, written = 0;
    u32 batch_crc = libk::kCrc32IeeeInit;
    for (u32 i = 0; i <= s.page_count; ++i)
        for (u32 part = 0; part < 2; ++part) {
            auto &record = *reinterpret_cast<Record *>(s.batch[batch_count]);
            libk::zero(record);
            record.lsn = s.checkpoint.lsn + written + batch_count + 1;
            record.type = kRecordType;
            record.target = u32(i == s.page_count ? 1 : s.page_ids[i]);
            const u16 offset = part ? kFragmentBytes : 0,
                      length = part ? kBlock - kFragmentBytes : kFragmentBytes;
            copy(record.data, &record.target, 4);
            copy(record.data + 4, &offset, 2);
            copy(record.data + 6, &length, 2);
            const auto *source = i == s.page_count ? reinterpret_cast<const u8 *>(&s.pending) : s.pages[i];
            copy(record.data + 8, source + offset, length);
            record.crc = crc(record.data, sizeof(record.data));
            batch_crc = libk::crc32_ieee_update(batch_crc, libk::as_bytes(record.data));
            ++batch_count;
            if (batch_count == kBatch || written + batch_count == records) {
                if (!raw_write(s, s.super.journal_start_block + written, batch_count, s.batch))
                    goto failed;
                written += batch_count;
                batch_count = 0;
            }
        }
    if (!flush(s))
        goto failed;
    {
        auto &c = *reinterpret_cast<Commit *>(s.journal);
        libk::zero(c);
        c.lsn = s.pending.lsn;
        c.first = s.checkpoint.lsn + 1;
        c.count = records;
        c.crc = ~batch_crc;
        if (!raw_write(s, s.super.journal_start_block + s.super.journal_block_count - 1, 1, &c) || !flush(s))
            goto failed;
    }
    // Journal is durable. Home blocks can now be replaced; replay is idempotent.
    for (u32 i = 0; i < s.page_count;) {
        u32 n = 1;
        while (i + n < s.page_count && n < kBatch && s.page_ids[i + n] == s.page_ids[i] + n)
            ++n;
        if (!raw_write(s, s.page_ids[i], n, s.pages[i]))
            goto failed;
        i += n;
    }
    if (!raw_write(s, 1, 1, &s.pending) || !flush(s))
        goto failed;
    s.checkpoint = s.pending;
    s.page_count = 0;
    s.transaction = false;
    return true;
failed:
    s.poisoned = true;
    s.page_count = 0;
    s.transaction = false;
    return false;
}
mx_i32_t finish(State &s) {
    if (!s.error)
        commit(s);
    const auto error = s.error;
    if (error)
        restore_extra(s);
    s.page_count = 0;
    s.transaction = false;
    return error;
}
bool put_inode(State &s, const InodeLeafEntry &inode) {
    return update(s, s.pending.inode_table_root, inode.inode_number, &inode, true);
}
u32 parent_of(const InodeLeafEntry &i) { return libk::load_le<u32>(i.reserved); }
bool valid_name(const char *name, u8 n) {
    if (!name || !n || n > 47 || (n == 1 && name[0] == '.') || (n == 2 && name[0] == '.' && name[1] == '.'))
        return false;
    for (u32 i = 0; i < n; ++i)
        if (!name[i] || name[i] == '/')
            return false;
    return true;
}
u64 dir_key(u32 parent, const char *name, u8 n) { return (u64(parent) << 32) | crc(name, n); }
// Collision-safe directory lookup. A rank seek skips subtrees by their maxima;
// only entries with the same hash need their actual names compared.
bool dir_find(State &s, const InodeLeafEntry &parent, const char *name, u8 n, DirLeafEntry &out,
              u64 *ordinal = nullptr) {
    u64 lo = 0, hi = count(s, parent.extent_tree_root);
    const auto target = dir_key(parent.inode_number, name, n);
    while (lo < hi) {
        u64 mid = (lo + hi) / 2;
        DirLeafEntry e{};
        if (!nth(s, parent.extent_tree_root, mid, false, &e))
            return fail(s);
        if (e.compound_key < target)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (u64 pos = lo;; ++pos) {
        DirLeafEntry e{};
        if (!nth(s, parent.extent_tree_root, pos, false, &e))
            return false;
        if (e.compound_key != target)
            return false;
        if (e.name_len == n && libk::str::equal_n(e.name, name, n)) {
            out = e;
            if (ordinal)
                *ordinal = pos;
            return true;
        }
    }
}
} // namespace
namespace {
bool roots_valid(State &s, const FsCheckpoint &cp) {
    const u64 roots[] = {cp.inode_table_root, cp.extent_bitmap_root, cp.phys_refcount_root,
                         cp.snapshot_tree_root};
    if (!roots[0] || !roots[1])
        return false;
    for (auto root : roots)
        if (root && (root < s.super.first_data_block || root >= s.super.total_blocks))
            return false;
    return cp.current_generation != 0;
}
bool recover_journal(State &s, bool checkpoint_ok) {
    if (!s.io.read(s.io.context, s.super.journal_start_block + s.super.journal_block_count - 1, 1, s.journal))
        return fail(s, MX_ERR_FAULT);
    const auto &c = *reinterpret_cast<const Commit *>(s.journal);
    const u64 lsn = c.lsn, first = c.first;
    const u32 records = c.count, expected = c.crc;
    // A device may persist the new checkpoint sector before metadata home
    // blocks in the same final flush. Equal-LSN redo must therefore replay
    // too. If a later transaction has overwritten this journal's fragments,
    // its invalid batch can be ignored: that later transaction only starts
    // after the preceding home/checkpoint flush has completed.
    if (checkpoint_ok && lsn < s.checkpoint.lsn)
        return true;
    if (!records || records % 2 || records > 2 * (kPages + 1) || records >= s.super.journal_block_count ||
        first > ~u64{0} - records || first + records != lsn)
        return checkpoint_ok || fail(s);
    s.page_count = 0;
    u32 batch_crc = libk::kCrc32IeeeInit;
    for (u32 i = 0; i < records; ++i) {
        if (!s.io.read(s.io.context, s.super.journal_start_block + i, 1, s.lookup))
            return fail(s, MX_ERR_FAULT);
        const auto &r = *reinterpret_cast<const Record *>(s.lookup);
        const u16 offset = libk::load_le<u16>(r.data + 4), length = libk::load_le<u16>(r.data + 6);
        const bool final = i / 2 == records / 2 - 1;
        if (r.lsn != first + i || r.type != kRecordType || r.reserved ||
            r.target != libk::load_le<u32>(r.data) || offset != (i % 2 ? kFragmentBytes : 0) ||
            length != (i % 2 ? kBlock - kFragmentBytes : kFragmentBytes) ||
            crc(r.data, sizeof(r.data)) != r.crc ||
            (final ? r.target != 1
                   : (r.target < s.super.first_data_block || r.target >= s.super.total_blocks)))
            goto invalid;
        if (!final) {
            if (i % 2) {
                if (s.page_ids[i / 2] != r.target)
                    goto invalid;
            } else {
                for (u32 j = 0; j < i / 2; ++j)
                    if (s.page_ids[j] == r.target)
                        goto invalid;
                s.page_ids[i / 2] = r.target;
            }
        }
        copy((final ? reinterpret_cast<u8 *>(&s.pending) : s.pages[i / 2]) + offset, r.data + 8, length);
        batch_crc = libk::crc32_ieee_update(batch_crc, libk::as_bytes(r.data));
    }
    if (~batch_crc != expected || !valid_checkpoint(s.pending) || s.pending.lsn != lsn ||
        !roots_valid(s, s.pending))
        goto invalid;
    s.page_count = records / 2 - 1;
    for (u32 i = 0; i < s.page_count; ++i)
        if (!raw_write(s, s.page_ids[i], 1, s.pages[i]))
            return false;
    if (!raw_write(s, 1, 1, &s.pending) || !flush(s))
        return false;
    s.checkpoint = s.pending;
    s.page_count = 0;
    return true;
invalid:
    s.page_count = 0;
    return checkpoint_ok || fail(s);
}
bool directory(State &s, u32 number, InodeLeafEntry &out) {
    if (!inode(s, number, out))
        return fail(s, MX_ERR_NOT_FOUND);
    if (out.type != FS_INODE_DIR)
        return fail(s, MX_ERR_BAD_TYPE);
    if (number < kDataRoot)
        return fail(s, MX_ERR_PERM);
    return true;
}
bool data_ref(State &s, u64 block, bool increment) {
    PhysExtentLeafEntry range{};
    u64 lo = 0, hi = count(s, s.pending.phys_refcount_root);
    while (lo < hi) {
        const auto mid = (lo + hi) / 2;
        PhysExtentLeafEntry row{};
        if (!nth(s, s.pending.phys_refcount_root, mid, false, &row))
            return fail(s);
        if (row.phys_block <= block)
            lo = mid + 1;
        else
            hi = mid;
    }
    const bool predecessor = lo && nth(s, s.pending.phys_refcount_root, lo - 1, false, &range);
    if (s.error)
        return false;
    const bool found =
        predecessor && block >= range.phys_block && block - range.phys_block < range.length_blocks;
    if (!found) {
        if (!increment)
            return fail(s);
        if (predecessor && range.refcount == 1 && block - range.phys_block == range.length_blocks) {
            ++range.length_blocks;
            return update(s, s.pending.phys_refcount_root, range.phys_block, &range);
        }
        PhysExtentLeafEntry fresh{block, 1, 1, 0, {}};
        return update(s, s.pending.phys_refcount_root, block, &fresh);
    }
    if (!range.refcount || (increment && range.refcount == ~u32{0}))
        return fail(s, MX_ERR_OVERFLOW);
    const auto end = range.phys_block + range.length_blocks;
    if (end < range.phys_block)
        return fail(s);
    if (range.phys_block != block || range.length_blocks != 1) {
        if (!update(s, s.pending.phys_refcount_root, range.phys_block, nullptr, false, true))
            return false;
        if (range.phys_block < block) {
            auto before = range;
            before.length_blocks = block - range.phys_block;
            if (!update(s, s.pending.phys_refcount_root, before.phys_block, &before))
                return false;
        }
        if (block + 1 < end) {
            auto after = range;
            after.phys_block = block + 1;
            after.length_blocks = end - block - 1;
            if (!update(s, s.pending.phys_refcount_root, after.phys_block, &after))
                return false;
        }
    }
    auto target = range;
    target.phys_block = block;
    target.length_blocks = 1;
    if (increment)
        ++target.refcount;
    else
        --target.refcount;
    if (target.refcount)
        return update(s, s.pending.phys_refcount_root, block, &target);
    if (range.phys_block == block && range.length_blocks == 1 &&
        !update(s, s.pending.phys_refcount_root, block, nullptr, false, true))
        return false;
    return release(s, block);
}
bool new_child(State &s, InodeLeafEntry &parent, const char *name, u8 n, u16 type, InodeLeafEntry &out) {
    DirLeafEntry dir{};
    if (dir_find(s, parent, name, n, dir))
        return fail(s, MX_ERR_BUSY);
    if (s.error)
        return false;
    if (s.next_inode == ~u32{0})
        return fail(s, MX_ERR_OVERFLOW);
    out = {};
    out.inode_number = s.next_inode++;
    out.type = type;
    out.inode_class = FS_CLASS_USER_DATA;
    out.nlink = 1;
    copy(out.reserved, &parent.inode_number, 4);
    dir.compound_key = dir_key(parent.inode_number, name, n);
    dir.child_inode = out.inode_number;
    dir.name_len = n;
    dir.file_type = u8(type);
    copy(dir.name, name, n);
    if (!put_inode(s, out) || !update(s, parent.extent_tree_root, dir.compound_key, &dir, false, false, 1))
        return false;
    if (type == FS_INODE_DIR)
        ++parent.nlink;
    return put_inode(s, parent);
}
} // namespace
bool mount(State &s, Io io) {
    libk::zero(s);
    s.io = io;
    if (!io.read || !io.write || !io.flush)
        return fail(s, MX_ERR_INVALID);
    if (!io.read(io.context, 0, 1, &s.super))
        return fail(s, MX_ERR_FAULT);
    const auto &b = s.super;
    if (b.magic != FS_MAGIC || b.version != FS_SUPERBLOCK_VERSION || b.block_size != kBlock ||
        b.total_blocks > ~u32{0} || b.journal_start_block != 2 || b.journal_block_count < 8 ||
        b.journal_block_count > b.total_blocks - 2 || b.first_data_block < 2 + b.journal_block_count ||
        b.first_data_block >= b.total_blocks)
        return fail(s);
    if (!io.read(io.context, 1, 1, &s.checkpoint))
        return fail(s, MX_ERR_FAULT);
    const bool good = valid_checkpoint(s.checkpoint) && roots_valid(s, s.checkpoint);
    if (!recover_journal(s, good))
        return false;
    // Do not silently overwrite a populated legacy checkpoint. The old service
    // remains available to read/export it before an explicit migration.
    if (!io.read(io.context, b.journal_start_block, 1, s.lookup))
        return fail(s, MX_ERR_FAULT);
    if (libk::load_le<u32>(s.lookup) == 0x44555231u)
        return fail(s, MX_ERR_NOT_IMPLEMENTED);
    s.pending = s.checkpoint;
    s.allocation_hint = b.first_data_block;
    s.mounted = true;
    if (libk::load_le<u32>(s.checkpoint.padding) == kExtension)
        s.next_inode = libk::load_le<u32>(s.checkpoint.padding + 4);
    else {
        InodeLeafEntry last{};
        if (!find(s, s.checkpoint.inode_table_root, ~u64{0}, true, &last, true))
            return fail(s);
        s.next_inode = last.inode_number < kDataRoot ? kDataRoot + 1 : last.inode_number + 1;
    }
    if (s.next_inode <= kDataRoot)
        return fail(s);
    restore_extra(s);
    for (u32 i = 0; i < s.reserve_count; ++i) {
        if (s.reserve[i] < b.first_data_block || s.reserve[i] >= b.total_blocks)
            return fail(s);
        for (u32 j = 0; j < i; ++j)
            if (s.reserve[j] == s.reserve[i])
                return fail(s);
    }
    if (!s.reserve_count) {
        if (!begin(s))
            return false;
        for (u32 i = 0; i < kReserve; ++i) {
            auto block = allocate(s);
            if (!block)
                return finish(s) == MX_OK;
            u8 *page = nullptr;
            u32 sh = 0, off = 0;
            if (!bitmap(s, block, page, sh, off))
                return finish(s) == MX_OK;
            page[off] |= u8(3u << sh);
            s.reserve[s.reserve_count++] = block;
        }
        if (finish(s) != MX_OK)
            return false;
    }
    InodeLeafEntry data{};
    if (!inode(s, kDataRoot, data)) {
        if (s.error || !begin(s))
            return false;
        data.inode_number = kDataRoot;
        data.type = FS_INODE_DIR;
        data.inode_class = FS_CLASS_USER_DATA;
        data.nlink = 1;
        if (!put_inode(s, data) || finish(s) != MX_OK)
            return false;
    } else if (data.type != FS_INODE_DIR || data.inode_class != FS_CLASS_USER_DATA)
        return fail(s);
    if (s.orphans) {
        u64 ordinal = 0;
        while (s.orphans) {
            InodeLeafEntry candidate{};
            if (!nth(s, s.checkpoint.inode_table_root, ordinal, true, &candidate))
                return fail(s);
            const bool reclaim =
                candidate.inode_number > kDataRoot && (!candidate.nlink || candidate.reserved[4]);
            if (reclaim) {
                if (reap(s, candidate.inode_number) != MX_OK)
                    return false;
                if (candidate.nlink)
                    ++ordinal;
            } else
                ++ordinal;
        }
    }
    return true;
}
bool inode(State &s, u32 number, InodeLeafEntry &out) {
    if (!s.mounted || s.poisoned)
        return fail(s, MX_ERR_SERVICE_UNAVAILABLE);
    if (!find(s, s.transaction ? s.pending.inode_table_root : s.checkpoint.inode_table_root, number, true,
              &out))
        return false;
    return out.inode_number == number && out.type && out.nlink;
}
mx_i32_t lookup(State &s, u32 parent, const char *name, u8 n, InodeLeafEntry &out) {
    s.error = 0;
    if (!valid_name(name, n))
        return MX_ERR_INVALID;
    InodeLeafEntry dir{};
    DirLeafEntry row{};
    if (!inode(s, parent, dir))
        return s.error ? s.error : MX_ERR_NOT_FOUND;
    if (dir.type != FS_INODE_DIR)
        return MX_ERR_BAD_TYPE;
    if (!dir_find(s, dir, name, n, row))
        return s.error ? s.error : MX_ERR_NOT_FOUND;
    return inode(s, row.child_inode, out) ? MX_OK : (s.error ? s.error : MX_ERR_BAD_IMAGE);
}
mx_i32_t create(State &s, u32 parent, const char *name, u8 n, u16 type, InodeLeafEntry &out) {
    if (!valid_name(name, n) || (type != FS_INODE_FILE && type != FS_INODE_DIR))
        return MX_ERR_INVALID;
    if (!begin(s))
        return finish(s);
    InodeLeafEntry dir{};
    if (directory(s, parent, dir))
        new_child(s, dir, name, n, type, out);
    return finish(s);
}
mx_i32_t read(State &s, u32 number, u64 offset, void *out, u32 length, u32 &received) {
    s.error = 0;
    received = 0;
    InodeLeafEntry file{};
    if (!out && length)
        return MX_ERR_INVALID;
    if (!inode(s, number, file))
        return s.error ? s.error : MX_ERR_NOT_FOUND;
    if (file.type != FS_INODE_FILE)
        return MX_ERR_BAD_TYPE;
    if (offset >= file.size)
        return MX_OK;
    if (length > file.size - offset)
        length = u32(file.size - offset);
    while (received < length) {
        u64 at = offset + received;
        u32 inside = u32(at % kBlock), n = kBlock - inside;
        if (n > length - received)
            n = length - received;
        ExtentLeafEntry extent{};
        if (find(s, file.extent_tree_root, at / kBlock, false, &extent)) {
            if (extent.length_blocks != 1 || extent.phys_block < s.super.first_data_block ||
                !get(s, extent.phys_block, s.data) || crc(s.data, kBlock) != extent.data_crc32)
                return MX_ERR_BAD_CHECKSUM;
            copy(static_cast<u8 *>(out) + received, s.data + inside, n);
        } else {
            if (s.error)
                return s.error;
            libk::zero(static_cast<u8 *>(out) + received, n);
        }
        received += n;
    }
    return MX_OK;
}
mx_i32_t write(State &s, u32 number, u64 offset, const void *input, u32 length) {
    if ((!input && length) || length > kBatch * kBlock || offset > ~u64{0} - length)
        return MX_ERR_INVALID;
    if (!begin(s))
        return finish(s);
    InodeLeafEntry file{};
    if (!inode(s, number, file)) {
        fail(s, MX_ERR_NOT_FOUND);
        return finish(s);
    }
    if (number < kDataRoot || file.inode_class != FS_CLASS_USER_DATA) {
        fail(s, MX_ERR_PERM);
        return finish(s);
    }
    if (file.type != FS_INODE_FILE) {
        fail(s, MX_ERR_BAD_TYPE);
        return finish(s);
    }
    u32 done = 0, buffered = 0;
    u64 first_buffered = 0;
    auto drain = [&] {
        if (!buffered)
            return true;
        const bool result = raw_write(s, first_buffered, buffered, s.batch);
        buffered = 0;
        return result;
    };
    while (done < length && !s.error) {
        const u64 at = offset + done, logical = at / kBlock;
        const u32 inside = u32(at % kBlock),
                  n = (length - done < kBlock - inside) ? length - done : kBlock - inside;
        ExtentLeafEntry old{};
        const bool exists = find(s, file.extent_tree_root, logical, false, &old);
        if (s.error)
            break;
        libk::zero(s.data, kBlock);
        if (exists) {
            if (old.length_blocks != 1 || !get(s, old.phys_block, s.data) ||
                crc(s.data, kBlock) != old.data_crc32) {
                fail(s, MX_ERR_BAD_CHECKSUM);
                break;
            }
        }
        // Sparse file holes and the unwritten tail of a partial block remain zero.
        copy(s.data + inside, static_cast<const u8 *>(input) + done, n);
        const auto physical = allocate(s);
        if (!physical)
            break;
        if (buffered && (buffered == kBatch || physical != first_buffered + buffered)) {
            if (!drain())
                break;
        }
        if (!buffered)
            first_buffered = physical;
        copy(s.batch[buffered++], s.data, kBlock);
        ExtentLeafEntry extent{logical, physical, 1, s.pending.current_generation, 0, 0, crc(s.data, kBlock),
                               0,       {}};
        if (!data_ref(s, physical, true) || !update(s, file.extent_tree_root, logical, &extent))
            break;
        if (exists && !data_ref(s, old.phys_block, false))
            break;
        done += n;
    }
    if (!s.error && drain()) {
        if (length && offset + length > file.size)
            file.size = offset + length;
        put_inode(s, file);
    }
    return finish(s);
}
mx_i32_t readdir(State &s, u32 number, u32 ordinal, DirLeafEntry &out, bool &eof) {
    s.error = 0;
    eof = false;
    InodeLeafEntry dir{};
    if (!inode(s, number, dir))
        return s.error ? s.error : MX_ERR_NOT_FOUND;
    if (dir.type != FS_INODE_DIR)
        return MX_ERR_BAD_TYPE;
    if (!nth(s, dir.extent_tree_root, ordinal, false, &out)) {
        eof = true;
        return s.error ? s.error : MX_OK;
    }
    if (!out.name_len || out.name_len > 47 || u32(out.compound_key >> 32) != number)
        return MX_ERR_BAD_IMAGE;
    return MX_OK;
}
mx_i32_t sync(State &s) {
    if (!s.mounted)
        return MX_ERR_NOT_IMPLEMENTED;
    if (s.poisoned)
        return MX_ERR_FAULT;
    s.error = 0;
    return flush(s) ? MX_OK : s.error;
}
} // namespace mochi::store

namespace mochi::store {
namespace {
bool raw_inode(State &s, u32 number, InodeLeafEntry &out) {
    return find(s, s.transaction ? s.pending.inode_table_root : s.checkpoint.inode_table_root, number, true,
                &out);
}
mx_i32_t reap(State &s, u32 number) {
    for (;;) {
        if (!begin(s))
            return finish(s);
        s.reclaim = true;
        InodeLeafEntry file{};
        if (!raw_inode(s, number, file)) {
            fail(s, MX_ERR_NOT_FOUND);
            return finish(s);
        }
        for (u32 n = 0; n < 32 && file.extent_tree_root && !s.error; ++n) {
            ExtentLeafEntry extent{};
            if (!nth(s, file.extent_tree_root, 0, false, &extent)) {
                fail(s);
                break;
            }
            if (file.type != FS_INODE_FILE || extent.length_blocks != 1) {
                fail(s);
                break;
            }
            if (!data_ref(s, extent.phys_block, false) ||
                !update(s, file.extent_tree_root, extent.logical_block, nullptr, false, true))
                break;
            if (s.reserve_count < kReserve)
                break;
        }
        const bool done = !file.extent_tree_root;
        if (!s.error) {
            if (done) {
                if (!s.orphans) {
                    fail(s);
                    return finish(s);
                }
                --s.orphans;
            }
            if (done && !file.nlink)
                update(s, s.pending.inode_table_root, number, nullptr, true, true);
            else {
                if (done)
                    file.reserved[4] = 0;
                put_inode(s, file);
            }
        }
        auto status = finish(s);
        if (status != MX_OK || done)
            return status;
    }
}
bool cycle(State &s, u32 source, u32 destination) {
    u32 slow = destination, fast = destination;
    auto advance = [&](u32 &number) {
        if (number == kDataRoot)
            return true;
        InodeLeafEntry ancestor{};
        if (!directory(s, number, ancestor))
            return false;
        number = parent_of(ancestor);
        if (number < kDataRoot)
            return fail(s);
        return true;
    };
    while (slow != kDataRoot) {
        if (slow == source || fast == source)
            return fail(s, MX_ERR_INVALID);
        if (!advance(slow) || !advance(fast))
            return false;
        if (fast == source)
            return fail(s, MX_ERR_INVALID);
        if (!advance(fast))
            return false;
        if (slow == fast && slow != kDataRoot)
            return fail(s);
    }
    return true;
}
} // namespace
mx_i32_t truncate(State &s, u32 number) {
    if (!begin(s))
        return finish(s);
    InodeLeafEntry file{};
    if (!inode(s, number, file))
        fail(s, MX_ERR_NOT_FOUND);
    else if (number < kDataRoot || file.inode_class != FS_CLASS_USER_DATA)
        fail(s, MX_ERR_PERM);
    else if (file.type != FS_INODE_FILE)
        fail(s, MX_ERR_BAD_TYPE);
    else {
        file.size = 0;
        if (!file.reserved[4])
            ++s.orphans;
        file.reserved[4] = 1;
        put_inode(s, file);
    }
    auto status = finish(s);
    return status == MX_OK ? reap(s, number) : status;
}
mx_i32_t unlink(State &s, u32 parent, const char *name, u8 n) {
    if (!valid_name(name, n))
        return MX_ERR_INVALID;
    if (!begin(s))
        return finish(s);
    s.reclaim = true;
    InodeLeafEntry dir{}, victim{};
    DirLeafEntry row{};
    u64 ordinal = 0;
    if (directory(s, parent, dir)) {
        if (!dir_find(s, dir, name, n, row, &ordinal) || !inode(s, row.child_inode, victim))
            fail(s, MX_ERR_NOT_FOUND);
        else if (victim.type == FS_INODE_DIR && count(s, victim.extent_tree_root))
            fail(s, MX_ERR_BUSY);
        else if (!s.error) {
            update(s, dir.extent_tree_root, ordinal, nullptr, false, true, 2);
            if (victim.type == FS_INODE_DIR)
                --dir.nlink;
            victim.nlink = 0;
            if (!victim.reserved[4])
                ++s.orphans;
            victim.reserved[4] = 1;
            put_inode(s, victim);
            put_inode(s, dir);
        }
    }
    const auto status = finish(s);
    return status == MX_OK ? reap(s, victim.inode_number) : status;
}
mx_i32_t rename(State &s, u32 old_parent, const char *old_name, u8 old_n, u32 new_parent,
                const char *new_name, u8 new_n) {
    if (!valid_name(old_name, old_n) || !valid_name(new_name, new_n))
        return MX_ERR_INVALID;
    if (!begin(s))
        return finish(s);
    InodeLeafEntry from{}, to{}, victim{};
    DirLeafEntry row{}, existing{};
    u64 ordinal = 0;
    if (directory(s, old_parent, from) && directory(s, new_parent, to)) {
        if (!dir_find(s, from, old_name, old_n, row, &ordinal) || !inode(s, row.child_inode, victim))
            fail(s, MX_ERR_NOT_FOUND);
        else if (old_parent == new_parent && old_n == new_n &&
                 libk::str::equal_n(old_name, new_name, old_n)) {
        } else if (dir_find(s, to, new_name, new_n, existing))
            fail(s, MX_ERR_BUSY);
        else if (!s.error && (victim.type != FS_INODE_DIR || cycle(s, victim.inode_number, new_parent))) {
            update(s, from.extent_tree_root, ordinal, nullptr, false, true, 2);
            if (victim.type == FS_INODE_DIR && old_parent != new_parent)
                --from.nlink;
            if (old_parent == new_parent)
                to = from;
            row.compound_key = dir_key(new_parent, new_name, new_n);
            row.name_len = new_n;
            libk::zero(row.name, sizeof(row.name));
            copy(row.name, new_name, new_n);
            update(s, to.extent_tree_root, row.compound_key, &row, false, false, 1);
            if (victim.type == FS_INODE_DIR && old_parent != new_parent)
                ++to.nlink;
            copy(victim.reserved, &new_parent, 4);
            if (old_parent != new_parent)
                put_inode(s, from);
            put_inode(s, to);
            put_inode(s, victim);
        }
    }
    return finish(s);
}
} // namespace mochi::store

namespace mochi::store {
mx_i32_t promote(State &s, u32 source_number, InodeLeafEntry &out) {
    s.error = 0;
    InodeLeafEntry source{};
    if (!inode(s, source_number, source))
        return s.error ? s.error : MX_ERR_NOT_FOUND;
    if (source.type != FS_INODE_FILE)
        return MX_ERR_BAD_TYPE;
    if (!begin(s))
        return finish(s);
    if (s.next_inode == ~u32{0}) {
        fail(s, MX_ERR_OVERFLOW);
        return finish(s);
    }
    out = {};
    out.inode_number = s.next_inode++;
    out.type = FS_INODE_FILE;
    out.inode_class = FS_CLASS_USER_DATA;
    out.reserved[4] = 1;
    const u32 parent = kDataRoot;
    copy(out.reserved, &parent, 4);
    ++s.orphans;
    put_inode(s, out);
    auto status = finish(s);
    if (status != MX_OK)
        return status;
    u64 ordinal = 0;
    for (;;) {
        ExtentLeafEntry first{};
        if (!nth(s, source.extent_tree_root, ordinal, false, &first)) {
            if (s.error)
                return s.error;
            break;
        }
        if (!begin(s))
            return finish(s);
        if (!raw_inode(s, out.inode_number, out)) {
            fail(s);
            return finish(s);
        }
        for (u32 n = 0; n < 32 && !s.error; ++n) {
            ExtentLeafEntry extent{};
            if (!nth(s, source.extent_tree_root, ordinal, false, &extent))
                break;
            if (extent.length_blocks != 1) {
                fail(s, MX_ERR_NOT_IMPLEMENTED);
                break;
            }
            if (!data_ref(s, extent.phys_block, true) ||
                !update(s, out.extent_tree_root, extent.logical_block, &extent))
                break;
            ++ordinal;
        }
        put_inode(s, out);
        status = finish(s);
        if (status != MX_OK)
            return status;
    }
    if (!begin(s))
        return finish(s);
    InodeLeafEntry root{};
    if (!directory(s, kDataRoot, root))
        return finish(s);
    char name[26]{};
    constexpr char prefix[] = "promoted-", hex[] = "0123456789abcdef";
    copy(name, prefix, 9);
    for (u32 i = 0; i < 8; ++i)
        name[9 + i] = hex[(source_number >> (28 - i * 4)) & 15];
    name[17] = '-';
    for (u32 i = 0; i < 8; ++i)
        name[18 + i] = hex[(out.inode_number >> (28 - i * 4)) & 15];
    DirLeafEntry row{};
    row.compound_key = dir_key(kDataRoot, name, sizeof(name));
    row.child_inode = out.inode_number;
    row.name_len = sizeof(name);
    row.file_type = FS_INODE_FILE;
    copy(row.name, name, sizeof(name));
    update(s, root.extent_tree_root, row.compound_key, &row, false, false, 1);
    out.nlink = 1;
    out.size = source.size;
    out.reserved[4] = 0;
    --s.orphans;
    put_inode(s, out);
    put_inode(s, root);
    return finish(s);
}
} // namespace mochi::store
