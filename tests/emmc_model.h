#pragma once
#include <array>
#include <cassert>
#include <cstring>
#include <vector>
#include "anko/emmc.h"
using namespace drivers::emmc;
struct Model {
    std::array<u8, 0x500> registers{};
    std::array<u8, 512> csd{};
    std::array<u8, 512 * 128> disk{};
    std::vector<u8> data, request;
    struct Cmd { u32 index, arg, partition; };
    std::vector<Cmd> commands;
    struct Transfer { u32 opcode, count; bool reliable; std::vector<u8> bytes; };
    std::vector<Transfer> transfers;
    u32 irq{}, command{}, argument{}, blocks{}, cursor{}, partition{}, block_count{};
    u16 type{2}, card_result{};
    u64 time{};
    bool reliable{}, writing{}, command_stall{}, data_stall{}, busy_stall{}, reset_stall{}, clock_stall{};
    bool error_with_ready{}, fail_restore{}, corrupt_response{}, byte_addressing{};
    bool corrupt_wide{}, fail_width_restore{};
    u32 fail_command{~0u}, fail_r1{~0u};
    alignas(4096) std::array<u8, kDmaArenaSize> dma{};
    static constexpr u32 dma_phys = 0x08000000;
    u32 dma_transfers{}, fifo_words{};
    bool adma_error{};
    Model() {
        put(0x29, 0x0b, 1); put(0xfe, 2, 2); put(0x3e, 8, 2); put(0x440, 0x40, 4);
        csd[168] = 0x20; csd[166] = 0x10; csd[179] = 0x48;
        csd[212] = 128;
        for (usize i = 0; i < disk.size(); ++i) disk[i] = u8(i * 7);
    }
    void put(u32 offset, u32 value, unsigned width) {
        for (unsigned i = 0; i < width; ++i) registers[offset + i] = u8(value >> (8 * i));
    }
    u32 get(u32 offset, unsigned width) const {
        u32 value = 0;
        for (unsigned i = 0; i < width; ++i) value |= u32(registers[offset + i]) << (8 * i);
        return value;
    }
    u32 read(u32 offset, unsigned width) {
        if (offset == 0x30) return irq;
        if (offset == 0x24) return busy_stall ? 2 : 0;
        if (offset == 0x20) {
            ++fifo_words;
            assert(width == 4 && !writing && cursor + 4 <= data.size());
            u32 word = 0;
            for (unsigned i = 0; i < 4; ++i) word |= u32(data[cursor++]) << (8 * i);
            advance(); return word;
        }
        return get(offset, width);
    }
    void advance() {
        if (cursor % 512) return;
        if (cursor < data.size()) irq |= writing ? 0x10 : 0x20;
        else {
            transfers.push_back({command, block_count, reliable, data});
            irq |= 2;
            if (writing && (partition & 7) == 0) {
                assert(argument * 512 + data.size() <= disk.size());
                std::memcpy(disk.data() + argument * 512, data.data(), data.size());
            }
            if (writing && (partition & 7) == 3) {
                assert(argument == 0 && data.size() >= 512);
                const u16 t = u16(data[510]) << 8 | data[511];
                if ((t & 0xfff) != 5) { type = t; request = data; }
                else assert(!reliable && data.size() == 512 && (t & 0xf000) == (type & 0xf000));
                assert((((t & 0xfff) == 1) || ((t & 0xfff) == 3)) == reliable);
            }
        }
    }
    void write(u32 offset, unsigned width, u32 value) {
        if (offset == 0x30) { irq &= ~value; return; }
        if (offset == 0x2f) { put(offset, reset_stall ? value : 0, width); irq = 0; return; }
        if (offset == 0x2c) { put(offset, value | ((value & 1) && !clock_stall ? 2 : 0), width); return; }
        if (offset == 0x20) {
            ++fifo_words;
            assert(width == 4 && writing && cursor + 4 <= data.size());
            for (unsigned i = 0; i < 4; ++i) data[cursor++] = u8(value >> (8 * i));
            advance(); return;
        }
        put(offset, value, width);
        if (offset != 0x0e) return;
        assert(width == 2);
        command = value >> 8; argument = get(8, 4);
        commands.push_back({command, argument, partition});
        irq = command_stall ? 0 : 1;
        put(0x10, 0x900, 4);
        if (command == fail_command) { irq |= 0x20000 | 0x8000; fail_command = ~0u; return; }
        if (command == fail_r1) { put(0x10, 1u << 22, 4); fail_r1 = ~0u; return; }
        switch (command) {
        case 0:
            csd[183] = csd[185] = csd[15] = 0;
            break;
        case 1: put(0x10, byte_addressing ? 0x80ff8000 : 0xc0ff8000, 4); break;
        case 6:
            assert((argument >> 24) == 3);
            if (((argument >> 16) & 255) == 32) { irq |= 2; break; } // FLUSH_CACHE
            if (((argument >> 16) & 255) == 183) {
                if (fail_width_restore && !((argument >> 8) & 255)) { irq |= 0x20000 | 0x8000; break; }
                csd[183] = u8(argument >> 8); irq |= 2; break;
            }
            assert(((argument >> 16) & 255) == 179);
            if (fail_restore && ((argument >> 8) & 7) != 3) {
                irq |= 0x20000 | 0x8000; break;
            }
            partition = (argument >> 8) & 255; csd[179] = u8(partition);
            irq |= 2; break;
        case 7: case 12: irq |= 2; break;
        case 23:
            blocks = argument & 0xffff; reliable = argument >> 31;
            break;
        case 8: case 18: case 25: {
            writing = command == 25; cursor = 0;
            block_count = get(6, 2);
            assert(get(4, 2) == 512);
            assert(!(get(0x0c, 2) & 0x0c)); // no automatic CMDs
            if (command != 8) assert(blocks == block_count);
            data.assign(block_count * 512, 0);
            if (command == 8) {
                std::memcpy(data.data(), csd.data(), 512);
                if (corrupt_wide && (get(0x28, 1) & 0x20)) data[212] ^= 1;
            }
            else if (!writing && (partition & 7) == 0) {
                assert(argument * 512 + data.size() <= disk.size());
                std::memcpy(data.data(), disk.data() + argument * 512, data.size());
            } else if (!writing) {
                assert((partition & 7) == 3 && argument == 0 && !request.empty());
                for (u32 f = 0; f < block_count; ++f) {
                    // Echo the nonce/payload as a recognizable response.
                    std::memcpy(data.data() + f * 512, request.data(), 512);
                    const u16 response = corrupt_response ? 0x0100 : u16((type & 0xf000) | ((type & 0xfff) << 8));
                    data[f * 512 + 510] = u8(response >> 8); data[f * 512 + 511] = u8(response);
                    data[f * 512 + 508] = u8(card_result >> 8); data[f * 512 + 509] = u8(card_result);
                }
            }
            if (get(0x0c, 2) & 1) {
                assert(command != 8 && (partition & 7) == 0);
                assert((get(0x28, 1) & 0x18) == 0x10 && get(0x58, 4) == dma_phys);
                ++dma_transfers;
                if (adma_error) { irq |= 0x02008000 | 2; break; }
                if (data_stall) break;
                u32 offset = 0;
                for (u32 n = 0; ; ++n) {
                    assert(n < 2);
                    auto* p = dma.data() + n * 8;
                    const u16 attr = u16(p[0]) | (u16(p[1]) << 8);
                    const u16 length = u16(p[2]) | (u16(p[3]) << 8);
                    u32 address = 0; for (unsigned b = 0; b < 4; ++b) address |= u32(p[4+b]) << (b*8);
                    assert((attr == 0x21 || attr == 0x23) && length && length <= 32768);
                    assert(address == dma_phys + kDmaDataOffset + offset && offset + length <= data.size());
                    if (writing) std::memcpy(data.data() + offset, dma.data() + kDmaDataOffset + offset, length);
                    else std::memcpy(dma.data() + kDmaDataOffset + offset, data.data() + offset, length);
                    offset += length;
                    if (attr & 2) break;
                }
                assert(offset == data.size());
                cursor = u32(data.size()); advance();
            } else if (!data_stall) irq |= writing ? 0x10 : 0x20;
            if (error_with_ready) { irq |= 0x200000 | 0x8000; error_with_ready = false; }
            break;
        }
        default: break;
        }
    }
    Io io() { return {this,
        [](void* c, u32 o, unsigned w) { return static_cast<Model*>(c)->read(o, w); },
        [](void* c, u32 o, unsigned w, u32 v) { static_cast<Model*>(c)->write(o, w, v); },
        [](void* c) { return static_cast<Model*>(c)->time += 100; }}; }
};

