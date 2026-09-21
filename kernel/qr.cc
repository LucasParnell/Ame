// kernel/qr.cc — QR Code Model 2 encoder (byte mode).
//
// Single-pass, allocation-free, no libc. The algorithm is the standard one:
// byte-mode bit stream -> Reed-Solomon blocks -> interleaved codewords ->
// function patterns -> data placement -> mask selection by ISO penalty ->
// format/version information. Feature values (EC block structure, alignment
// centres, format/version words) come from the generated tables in
// qr_tables.h, which are produced from kernel/assets/qr_tables.json.
//
// Not reentrant: the working matrix and codeword buffers are static so the
// encoder never touches the faulting thread's stack. Only the panic path uses it.

#include "anko/qr.h"

#include "qr_tables.h"

namespace anko::qr {
namespace {

using tables::EcGroup;

constexpr u32 kTotalCells = kMaxSize * kMaxSize;

// Matrix cell encoding: 0/1 = data module (light/dark), 2/3 = function module.
constexpr u8 kDataLight = 0;
constexpr u8 kDataDark = 1;
constexpr u8 kFuncLight = 2;
constexpr u8 kFuncDark = 3;

inline bool is_function(u8 cell) { return cell >= kFuncLight; }
inline bool is_dark_cell(u8 cell) { return (cell & 1u) != 0; }

u8 g_cells[kTotalCells];             // working matrix
static u8 g_interleaved[kMaxCodewords];  // final codeword sequence (static: not on a faulting stack)
u8 g_codewords[kMaxCodewords];       // message + EC codewords
u8 g_ec[kMaxBlocks][30];             // per-block EC bytes (max EC per block = 30)
u8 g_gen[30];                        // Reed-Solomon generator coefficients

// --- bit stream ------------------------------------------------------------

struct BitSink {
    u8* bytes;
    u32 capacity_bits;
    u32 bit_len = 0;

    void put(u32 value, u32 count) {
        for (u32 i = 0; i < count; ++i) {
            const u32 bit = (value >> (count - 1u - i)) & 1u;
            if (bit_len < capacity_bits && bit) {
                bytes[bit_len >> 3] |= static_cast<u8>(0x80u >> (bit_len & 7u));
            }
            ++bit_len;
        }
    }
};

// --- Reed-Solomon over GF(256), primitive polynomial 0x11D -----------------

u8 gf_mul(u8 x, u8 y) {
    u8 z = 0;
    for (int i = 7; i >= 0; --i) {
        z = static_cast<u8>((z << 1) ^ ((z >> 7) * 0x1Du));
        z ^= static_cast<u8>(((y >> i) & 1u) * x);
    }
    return z;
}

// Generator polynomial of `degree`: coefficients for all but the leading 1.
void rs_generator(u32 degree) {
    for (u32 i = 0; i + 1 < degree; ++i) g_gen[i] = 0;
    g_gen[degree - 1] = 1;
    u8 root = 1;
    for (u32 i = 0; i < degree; ++i) {
        for (u32 j = 0; j < degree; ++j) {
            g_gen[j] = gf_mul(g_gen[j], root);
            if (j + 1 < degree) g_gen[j] ^= g_gen[j + 1];
        }
        root = gf_mul(root, 0x02);
    }
}

void rs_remainder(const u8* data, u32 len, u32 degree, u8* result) {
    for (u32 i = 0; i < degree; ++i) result[i] = 0;
    for (u32 i = 0; i < len; ++i) {
        const u8 factor = static_cast<u8>(data[i] ^ result[0]);
        for (u32 j = 0; j + 1 < degree; ++j) result[j] = result[j + 1];
        result[degree - 1] = 0;
        for (u32 j = 0; j < degree; ++j) result[j] ^= gf_mul(g_gen[j], factor);
    }
}

// --- feature tables --------------------------------------------------------

u32 data_codewords(u32 version, Level level) {
    const tables::EcSpec& spec = tables::kEc[version - 1][static_cast<u32>(level)];
    u32 total = 0;
    for (u32 g = 0; g < spec.group_count; ++g) {
        total += static_cast<u32>(spec.groups[g].blocks) * spec.groups[g].data_bytes;
    }
    return total;
}

u8 level_code(Level level) {
    switch (level) {
    case Level::L: return 1;
    case Level::M: return 0;
    case Level::Q: return 3;
    case Level::H: return 2;
    }
    return 0;
}

u32 char_count_bits(u32 version) { return version <= 9 ? 8u : 16u; }

// --- matrix helpers --------------------------------------------------------

void set_function(u32 x, u32 y, u32 size, bool dark) {
    if (x >= size || y >= size) return;
    g_cells[y * size + x] = dark ? kFuncDark : kFuncLight;
}

void draw_finder(u32 cx, u32 cy, u32 size) {
    // 7x7 finder plus its one-module separator: dark where the Chebyshev
    // distance is 0/1 (core) or 3 (outer ring), light at 2 and 4.
    for (int dy = -4; dy <= 4; ++dy) {
        for (int dx = -4; dx <= 4; ++dx) {
            const int ax = dx < 0 ? -dx : dx;
            const int ay = dy < 0 ? -dy : dy;
            const int d = ax > ay ? ax : ay;
            const int x = static_cast<int>(cx) + dx;
            const int y = static_cast<int>(cy) + dy;
            if (x < 0 || y < 0 || x >= static_cast<int>(size) || y >= static_cast<int>(size)) continue;
            set_function(static_cast<u32>(x), static_cast<u32>(y), size, d != 2 && d != 4);
        }
    }
}

void draw_function_patterns(u32 version, u32 size) {
    for (u32 i = 0; i < kTotalCells; ++i) g_cells[i] = kDataLight;

    // Finders + separators at three corners (drawn as function modules).
    draw_finder(3, 3, size);
    draw_finder(size - 4, 3, size);
    draw_finder(3, size - 4, size);

    // Timing patterns run between the finders (row/column 6, inside the
    // separators) so they never overwrite a finder ring.
    for (u32 i = 8; i + 8 < size; ++i) {
        set_function(6, i, size, (i % 2) == 0);
        set_function(i, 6, size, (i % 2) == 0);
    }

    // Alignment patterns.
    const u32 count = tables::kAlignCount[version - 1];
    const u8* pos = tables::kAlign[version - 1];
    for (u32 a = 0; a < count; ++a) {
        for (u32 b = 0; b < count; ++b) {
            const u32 cx = pos[a];
            const u32 cy = pos[b];
            if ((cx == 6 && cy == 6) || (cx == 6 && cy == size - 7) ||
                (cx == size - 7 && cy == 6)) {
                continue; // overlaps a finder
            }
            for (int dy = -2; dy <= 2; ++dy) {
                for (int dx = -2; dx <= 2; ++dx) {
                    const int d = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy)
                                      ? (dx < 0 ? -dx : dx)
                                      : (dy < 0 ? -dy : dy);
                    set_function(cx + dx, cy + dy, size, d != 1);
                }
            }
        }
    }

    // Dark module and reserved format/version areas.
    set_function(8, size - 8, size, true);
    for (u32 i = 0; i < 9; ++i) {
        if (i != 6) {
            set_function(8, i, size, false);
            set_function(i, 8, size, false);
        }
    }
    for (u32 i = 0; i < 8; ++i) {
        set_function(size - 1 - i, 8, size, false);
        set_function(8, size - 1 - i, size, false);
    }
}

void draw_format_bits(Level level, u32 mask, u32 size) {
    const u16 bits = tables::kFormatInfo[(static_cast<u32>(level_code(level)) << 3) | mask];
    for (u32 i = 0; i < 6; ++i) set_function(8, i, size, ((bits >> i) & 1u) != 0);
    set_function(8, 7, size, ((bits >> 6) & 1u) != 0);
    set_function(8, 8, size, ((bits >> 7) & 1u) != 0);
    set_function(7, 8, size, ((bits >> 8) & 1u) != 0);
    for (u32 i = 9; i < 15; ++i) set_function(14 - i, 8, size, ((bits >> i) & 1u) != 0);
    for (u32 i = 0; i < 8; ++i) set_function(size - 1 - i, 8, size, ((bits >> i) & 1u) != 0);
    for (u32 i = 8; i < 15; ++i) set_function(8, size - 15 + i, size, ((bits >> i) & 1u) != 0);
    set_function(8, size - 8, size, true);
}

void draw_version_bits(u32 version, u32 size) {
    if (version < 7) return;
    const u32 bits = tables::kVersionInfo[version - 1];
    for (u32 i = 0; i < 18; ++i) {
        const bool dark = ((bits >> i) & 1u) != 0;
        const u32 a = size - 11 + (i % 3);
        const u32 b = i / 3;
        set_function(a, b, size, dark);
        set_function(b, a, size, dark);
    }
}

void place_data(u32 size, u32 bit_count) {
    u32 bit = 0;
    for (int right = static_cast<int>(size) - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5; // skip the vertical timing column
        for (u32 vert = 0; vert < size; ++vert) {
            for (u32 j = 0; j < 2; ++j) {
                const u32 x = static_cast<u32>(right - static_cast<int>(j));
                const bool upward = ((static_cast<u32>(right) + 1u) & 2u) == 0;
                const u32 y = upward ? size - 1u - vert : vert;
                u8& cell = g_cells[y * size + x];
                if (is_function(cell)) continue;
                bool dark = false;
                if (bit < bit_count) {
                    dark = ((g_codewords[bit >> 3] >> (7u - (bit & 7u))) & 1u) != 0;
                    ++bit;
                }
                cell = dark ? kDataDark : kDataLight;
            }
        }
    }
}

bool mask_bit(u32 mask, u32 x, u32 y) {
    switch (mask) {
    case 0: return ((y + x) % 2u) == 0;
    case 1: return (y % 2u) == 0;
    case 2: return (x % 3u) == 0;
    case 3: return ((y + x) % 3u) == 0;
    case 4: return ((y / 2u + x / 3u) % 2u) == 0;
    case 5: return ((y * x) % 2u + (y * x) % 3u) == 0;
    case 6: return (((y * x) % 2u + (y * x) % 3u) % 2u) == 0;
    default: return (((y + x) % 2u + (y * x) % 3u) % 2u) == 0;
    }
}

void apply_mask(u32 mask, u32 size) {
    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            u8& cell = g_cells[y * size + x];
            if (!is_function(cell) && mask_bit(mask, x, y)) {
                cell = static_cast<u8>(cell ^ 1u);
            }
        }
    }
}

// ISO/IEC 18004 penalty rules 1-4.
u32 penalty(u32 size) {
    u32 score = 0;

    // Rule 1: runs of five or more same-colour modules.
    for (u32 y = 0; y < size; ++y) {
        u32 run = 1;
        for (u32 x = 1; x < size; ++x) {
            if (is_dark_cell(g_cells[y * size + x]) == is_dark_cell(g_cells[y * size + x - 1])) {
                ++run;
            } else {
                if (run >= 5) score += 3u + (run - 5u);
                run = 1;
            }
        }
        if (run >= 5) score += 3u + (run - 5u);
    }
    for (u32 x = 0; x < size; ++x) {
        u32 run = 1;
        for (u32 y = 1; y < size; ++y) {
            if (is_dark_cell(g_cells[y * size + x]) == is_dark_cell(g_cells[(y - 1) * size + x])) {
                ++run;
            } else {
                if (run >= 5) score += 3u + (run - 5u);
                run = 1;
            }
        }
        if (run >= 5) score += 3u + (run - 5u);
    }

    // Rule 2: 2x2 blocks of one colour.
    for (u32 y = 0; y + 1 < size; ++y) {
        for (u32 x = 0; x + 1 < size; ++x) {
            const bool a = is_dark_cell(g_cells[y * size + x]);
            if (a == is_dark_cell(g_cells[y * size + x + 1]) &&
                a == is_dark_cell(g_cells[(y + 1) * size + x]) &&
                a == is_dark_cell(g_cells[(y + 1) * size + x + 1])) {
                score += 3;
            }
        }
    }

    // Rule 3: finder-like 1:1:3:1:1 patterns with a 4-module light margin, in
    // both orientations (the 11-bit patterns 10111010000 and its reverse).
    static const u8 pattern_a[11] = {1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0};
    static const u8 pattern_b[11] = {0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1};
    const u8* patterns[2] = {pattern_a, pattern_b};
    auto matches = [&](u32 x, u32 y, int dx, int dy, const u8* pattern) {
        for (u32 i = 0; i < 11; ++i) {
            const int cx = static_cast<int>(x) + dx * static_cast<int>(i);
            const int cy = static_cast<int>(y) + dy * static_cast<int>(i);
            if (cx < 0 || cy < 0 || cx >= static_cast<int>(size) || cy >= static_cast<int>(size)) {
                return false;
            }
            if (is_dark_cell(g_cells[static_cast<u32>(cy) * size + static_cast<u32>(cx)]) !=
                (pattern[i] != 0)) {
                return false;
            }
        }
        return true;
    };
    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            for (u32 p = 0; p < 2; ++p) {
                if (matches(x, y, 1, 0, patterns[p]) || matches(x, y, 0, 1, patterns[p])) {
                    score += 40;
                }
            }
        }
    }

    // Rule 4: deviation from 50% dark.
    u32 dark = 0;
    for (u32 i = 0; i < size * size; ++i) {
        if (is_dark_cell(g_cells[i])) ++dark;
    }
    const u32 total = size * size;
    const u32 percent = (dark * 100u) / total;
    const u32 k = percent > 50u ? percent - 50u : 50u - percent;
    score += (k / 5u) * 10u;
    return score;
}

} // namespace

u32 fit_version(u32 len, Level level) {
    for (u32 version = 1; version <= kMaxVersion; ++version) {
        const u32 bits = 4u + char_count_bits(version) + 8u * len;
        if (data_codewords(version, level) >= (bits + 7u) / 8u) return version;
    }
    return 0;
}

bool encode(const u8* data, u32 len, Level level, Code& out) {
    if (!data && len != 0) return false;
    const u32 version = fit_version(len, level);
    if (version == 0) return false;
    const u32 size = 17u + 4u * version;
    const tables::EcSpec& spec = tables::kEc[version - 1][static_cast<u32>(level)];

    u32 data_cw = 0;
    u32 total_cw = 0;
    u32 max_data_per_block = 0;
    for (u32 g = 0; g < spec.group_count; ++g) {
        const EcGroup& grp = spec.groups[g];
        data_cw += static_cast<u32>(grp.blocks) * grp.data_bytes;
        total_cw += static_cast<u32>(grp.blocks) * (grp.data_bytes + grp.ec_bytes);
        if (grp.data_bytes > max_data_per_block) max_data_per_block = grp.data_bytes;
    }
    if (total_cw > kMaxCodewords || data_cw > kMaxCodewords) return false;

    // --- byte-mode bit stream, padded to the data-codeword capacity ---------
    for (u32 i = 0; i < data_cw; ++i) g_codewords[i] = 0;
    BitSink sink{g_codewords, data_cw * 8u};
    sink.put(0x4u, 4);                       // byte mode indicator
    sink.put(len, char_count_bits(version)); // character count
    for (u32 i = 0; i < len; ++i) sink.put(data[i], 8);
    const u32 capacity_bits = data_cw * 8u;
    u32 terminator = capacity_bits > sink.bit_len ? capacity_bits - sink.bit_len : 0;
    if (terminator > 4u) terminator = 4u;
    sink.put(0, terminator);
    if (sink.bit_len > capacity_bits) return false; // cannot happen after fit_version
    while (sink.bit_len % 8u != 0) sink.put(0, 1);
    for (u32 i = sink.bit_len / 8u, pad = 0; i < data_cw; ++i, ++pad) {
        g_codewords[i] = (pad % 2u == 0) ? 0xECu : 0x11u;
    }

    // --- error correction, per block ---------------------------------------
    rs_generator(spec.groups[0].ec_bytes);
    {
        u32 offset = 0;
        u32 block = 0;
        for (u32 g = 0; g < spec.group_count; ++g) {
            for (u32 b = 0; b < spec.groups[g].blocks; ++b, ++block) {
                if (block >= kMaxBlocks) return false;
                rs_remainder(g_codewords + offset, spec.groups[g].data_bytes,
                             spec.groups[g].ec_bytes, g_ec[block]);
                offset += spec.groups[g].data_bytes;
            }
        }
    }

    // --- interleave data then EC codewords ---------------------------------
    {
        u8* interleaved = g_interleaved;
        u32 out_len = 0;
        u32 offset = 0;
        u32 block = 0;
        u32 block_start[kMaxBlocks];
        u32 block_data[kMaxBlocks];
        for (u32 g = 0; g < spec.group_count; ++g) {
            for (u32 b = 0; b < spec.groups[g].blocks; ++b, ++block) {
                block_start[block] = offset;
                block_data[block] = spec.groups[g].data_bytes;
                offset += spec.groups[g].data_bytes;
            }
        }
        for (u32 i = 0; i < max_data_per_block; ++i) {
            for (u32 b = 0; b < block; ++b) {
                if (i < block_data[b]) interleaved[out_len++] = g_codewords[block_start[b] + i];
            }
        }
        for (u32 i = 0; i < spec.groups[0].ec_bytes; ++i) {
            for (u32 b = 0; b < block; ++b) interleaved[out_len++] = g_ec[b][i];
        }
        if (out_len != total_cw) return false;
        for (u32 i = 0; i < total_cw; ++i) g_codewords[i] = interleaved[i];
    }

    // --- matrix ------------------------------------------------------------
    draw_function_patterns(version, size);
    draw_format_bits(level, 0, size);       // reserve format areas before data
    draw_version_bits(version, size);
    place_data(size, total_cw * 8u);

    u32 best_mask = 0;
    u32 best_penalty = 0xFFFFFFFFu;
    for (u32 mask = 0; mask < 8; ++mask) {
        apply_mask(mask, size);
        draw_format_bits(level, mask, size);
        const u32 p = penalty(size);
        if (p < best_penalty) {
            best_penalty = p;
            best_mask = mask;
        }
        apply_mask(mask, size); // undo
    }
    apply_mask(best_mask, size);
    draw_format_bits(level, best_mask, size);

    // --- publish -----------------------------------------------------------
    out.version = version;
    out.size = size;
    out.mask = best_mask;
    for (u32 i = 0; i < kModuleBytes; ++i) out.modules[i] = 0;
    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            if (is_dark_cell(g_cells[y * size + x])) {
                const u32 bit = y * size + x;
                out.modules[bit >> 3] |= static_cast<u8>(0x80u >> (bit & 7u));
            }
        }
    }
    return true;
}

bool dark(const Code& code, u32 x, u32 y) {
    if (x >= code.size || y >= code.size) return false;
    const u32 bit = y * code.size + x;
    return ((code.modules[bit >> 3] >> (7u - (bit & 7u))) & 1u) != 0;
}

} // namespace anko::qr
