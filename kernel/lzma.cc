// kernel/lzma.cc — minimal LZMA1 encoder (raw stream; see lzma.h for parameters).
//
// Implements the parts of the format a decoder needs: the binary range coder,
// the literal coder (including the "matched literal" path the decoder uses after
// a match), the length coder and the distance coder (position slots, the
// reversed "spec pos" tree for slots < 14, direct bits + align otherwise).
// Matching is greedy: the encoder picks the longest match it can find in the
// window and otherwise emits a literal. There is no optimal parsing and no rep
// distances — that costs a little ratio and keeps the code small and auditable,
// which matters for something running in a panic handler.

#include "anko/lzma.h"

namespace anko::lzma {
namespace {

constexpr u32 kNumStates = 12;
constexpr u32 kNumPosStates = 1u << kPb;             // 4
constexpr u32 kNumLitContexts = 1u << kLc;           // 8
constexpr u32 kLitProbs = 0x300;
constexpr u32 kAlignBits = 4;
constexpr u32 kAlignSize = 1u << kAlignBits;         // 16
constexpr u32 kEndPosModelIndex = 14;
constexpr u32 kNumFullDistances = 1u << (kEndPosModelIndex >> 1);   // 128
constexpr u32 kNumPosSlotProbs = 64;
constexpr u32 kSpecPosProbs = kNumFullDistances - kEndPosModelIndex; // 114
constexpr u32 kNumLenToPosStates = 4;
constexpr u32 kProbInit = 1024;
constexpr u32 kMoveBits = 5;

// Static probability tables: the encoder is never run concurrently, and nothing
// may allocate on the panic path.
u16 g_is_match[kNumStates][kNumPosStates];
u16 g_is_rep[kNumStates];
u16 g_literal[kNumLitContexts][kLitProbs];
u16 g_len_choice;
u16 g_len_choice2;
u16 g_len_low[kNumPosStates][8];
u16 g_len_mid[kNumPosStates][8];
u16 g_len_high[256];
u16 g_pos_slot[kNumLenToPosStates][kNumPosSlotProbs];
u16 g_spec_pos[kSpecPosProbs];
u16 g_align[kAlignSize];

const u8* g_data = nullptr;

struct RangeEncoder {
    u64 low = 0;
    u32 range = 0xFFFFFFFFu;
    u8 cache = 0;
    u64 cache_size = 1;
    u8* out = nullptr;
    u32 cap = 0;
    u32 len = 0;
    bool overflow = false;

    void put(u8 byte) {
        if (len < cap) {
            out[len] = byte;
        } else {
            overflow = true;
        }
        ++len;
    }

    // Carry-propagating low-word output.
    void shift_low() {
        if (static_cast<u32>(low) < 0xFF000000u || (low >> 32) != 0) {
            u8 temp = cache;
            do {
                put(static_cast<u8>(temp + static_cast<u8>(low >> 32)));
                temp = 0xFF;
            } while (--cache_size != 0);
            cache = static_cast<u8>(low >> 24);
        }
        ++cache_size;
        low = static_cast<u64>(static_cast<u32>(low) << 8);
    }

    void encode_bit(u16* prob, u32 bit) {
        const u32 bound = (range >> 11) * static_cast<u32>(*prob);
        if (bit == 0) {
            range = bound;
            *prob = static_cast<u16>(*prob + ((2048u - *prob) >> kMoveBits));
        } else {
            low += bound;
            range -= bound;
            *prob = static_cast<u16>(*prob - (*prob >> kMoveBits));
        }
        while (range < (1u << 24)) {
            range <<= 8;
            shift_low();
        }
    }

    void encode_direct(u32 value, u32 nbits) {
        for (u32 i = nbits; i > 0; --i) {
            range >>= 1;
            if ((value >> (i - 1)) & 1u) low += range;
            while (range < (1u << 24)) {
                range <<= 8;
                shift_low();
            }
        }
    }

    void flush() {
        for (u32 i = 0; i < 5; ++i) shift_low();
    }
};

void reset_probs() {
    for (auto& row : g_is_match) for (u16& p : row) p = kProbInit;
    for (u16& p : g_is_rep) p = kProbInit;
    for (auto& row : g_literal) for (u16& p : row) p = kProbInit;
    g_len_choice = g_len_choice2 = kProbInit;
    for (auto& row : g_len_low) for (u16& p : row) p = kProbInit;
    for (auto& row : g_len_mid) for (u16& p : row) p = kProbInit;
    for (u16& p : g_len_high) p = kProbInit;
    for (auto& row : g_pos_slot) for (u16& p : row) p = kProbInit;
    for (u16& p : g_spec_pos) p = kProbInit;
    for (u16& p : g_align) p = kProbInit;
}

void tree_encode(RangeEncoder& rc, u16* probs, u32 nbits, u32 symbol) {
    u32 m = 1;
    for (u32 i = nbits; i > 0; --i) {
        const u32 bit = (symbol >> (i - 1)) & 1u;
        rc.encode_bit(&probs[m], bit);
        m = (m << 1) | bit;
    }
}

void tree_reverse_encode(RangeEncoder& rc, u16* probs, u32 nbits, u32 symbol) {
    u32 m = 1;
    for (u32 i = 0; i < nbits; ++i) {
        const u32 bit = symbol & 1u;
        symbol >>= 1;
        rc.encode_bit(&probs[m], bit);
        m = (m << 1) | bit;
    }
}

void encode_literal(RangeEncoder& rc, u32 state, u32 pos, u32 rep0) {
    const u32 prev_byte = pos > 0 ? g_data[pos - 1] : 0;
    const u32 ctx = ((pos & ((1u << kLp) - 1u)) << kLc) + (prev_byte >> (8 - kLc));
    u16* probs = g_literal[ctx];
    u32 symbol = static_cast<u32>(g_data[pos]) | 0x100u;
    if (state < 7) {
        do {
            rc.encode_bit(&probs[symbol >> 8], (symbol >> 7) & 1u);
            symbol <<= 1;
        } while (symbol < 0x10000u);
    } else {
        // Matched literal: the decoder walks the same tree using the byte at
        // (pos - rep0 - 1) as context.
        const u32 match_index = pos - rep0 - 1u;
        u32 match_byte = match_index < pos ? static_cast<u32>(g_data[match_index]) : 0u;
        u32 offs = 0x100;
        do {
            match_byte <<= 1;
            rc.encode_bit(&probs[offs + (match_byte & offs) + (symbol >> 8)], (symbol >> 7) & 1u);
            symbol <<= 1;
            offs &= ~(match_byte ^ symbol);
        } while (symbol < 0x10000u);
    }
}

void encode_len(RangeEncoder& rc, u32 len, u32 pos_state) {
    len -= kMinMatch;
    if (len < 8) {
        rc.encode_bit(&g_len_choice, 0);
        tree_encode(rc, g_len_low[pos_state], 3, len);
        return;
    }
    rc.encode_bit(&g_len_choice, 1);
    len -= 8;
    if (len < 8) {
        rc.encode_bit(&g_len_choice2, 0);
        tree_encode(rc, g_len_mid[pos_state], 3, len);
        return;
    }
    rc.encode_bit(&g_len_choice2, 1);
    tree_encode(rc, g_len_high, 8, len - 8);
}

u32 pos_slot_of(u32 dist) {
    if (dist < 4) return dist;
    u32 bits = 0;
    while ((dist >> bits) > 1u) ++bits;   // index of the highest set bit
    return (bits << 1) + ((dist >> (bits - 1)) & 1u);
}

void encode_distance(RangeEncoder& rc, u32 dist, u32 len_to_pos_state) {
    const u32 slot = pos_slot_of(dist);
    tree_encode(rc, g_pos_slot[len_to_pos_state], 6, slot);
    if (slot < 4) return;
    const u32 footer = (slot >> 1) - 1u;
    const u32 base = (2u | (slot & 1u)) << footer;
    const u32 reduced = dist - base;
    if (slot < kEndPosModelIndex) {
        tree_reverse_encode(rc, g_spec_pos + base - slot, footer, reduced);
    } else {
        rc.encode_direct(reduced >> kAlignBits, footer - kAlignBits);
        tree_reverse_encode(rc, g_align, kAlignBits, reduced & (kAlignSize - 1u));
    }
}

} // namespace

u32 bound(u32 input_len) { return input_len + input_len / 8u + 64u; }

u32 compress(const u8* data, u32 len, u8* out, u32 cap) {
    if (!data || len == 0 || !out || cap == 0) return 0;
    g_data = data;
    reset_probs();

    RangeEncoder rc;
    rc.out = out;
    rc.cap = cap;

    u32 state = 0;
    u32 rep0 = 0; // last distance - 1
    u32 pos = 0;
    while (pos < len) {
        const u32 pos_state = pos & (kNumPosStates - 1u);
        const u32 max_len = (len - pos) < kMaxMatch ? (len - pos) : kMaxMatch;
        const u32 window = pos < kDictSize ? pos : kDictSize;

        // Greedy longest match; `back` is the distance, `l` may exceed it (the
        // decoder copies byte-wise, so overlapping matches are legal).
        u32 best_len = 0;
        u32 best_dist = 0;
        for (u32 back = 1; back <= window; ++back) {
            u32 l = 0;
            while (l < max_len && data[pos - back + l] == data[pos + l]) ++l;
            if (l > best_len) {
                best_len = l;
                best_dist = back;
                if (l == max_len) break;
            }
        }

        if (best_len >= 3 && best_len <= kMaxMatch) {
            rc.encode_bit(&g_is_match[state][pos_state], 1);
            rc.encode_bit(&g_is_rep[state], 0);
            encode_len(rc, best_len, pos_state);
            const u32 lps = (best_len - kMinMatch) < 3u ? (best_len - kMinMatch) : 3u;
            encode_distance(rc, best_dist - 1u, lps);
            state = state < 7 ? 7 : 10;
            rep0 = best_dist - 1u;
            pos += best_len;
        } else {
            rc.encode_bit(&g_is_match[state][pos_state], 0);
            encode_literal(rc, state, pos, rep0);
            if (state < 4) {
                state = 0;
            } else if (state < 10) {
                state -= 3;
            } else {
                state -= 6;
            }
            ++pos;
        }
    }

    rc.flush();
    if (rc.overflow) return 0;
    return rc.len;
}

} // namespace anko::lzma
