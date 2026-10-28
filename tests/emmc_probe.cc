// Executable SDHCI/card model: runs production PIO, ADMA2 and RPMB bridge.
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "anko/emmc.h"
#include "emmc.h"

using namespace drivers::emmc;
#include "emmc_model.h"

static Frame frame(u16 type, u16 address = 0, u16 count = 0) {
    Frame f{};
    f.bytes[510] = u8(type >> 8); f.bytes[511] = u8(type);
    f.bytes[504] = u8(address >> 8); f.bytes[505] = u8(address);
    f.bytes[506] = u8(count >> 8); f.bytes[507] = u8(count);
    f.bytes[484] = 0x76; // nonce survives PIO and bridge copies
    return f;
}
static void le32(u8* p, u32 value) { for (unsigned i = 0; i < 4; ++i) p[i] = u8(value >> (8 * i)); }

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string scenario(argv[1]);
    Model m;
    Driver driver(m.io());
    if (scenario == "power-off") { m.put(0x29, 0, 1); assert(driver.initialize(200000000) == Error::NotReady); return 0; }
    if (scenario == "clock-timeout") m.clock_stall = true;
    if (scenario == "reset-timeout") m.reset_stall = true;
    if (scenario == "command-timeout") m.command_stall = true;
    if (scenario == "byte-addressing") m.byte_addressing = true;
    if (scenario == "native-4k") m.csd[61] = 1;
    if (scenario == "cache-flush" || scenario == "buffered-flush") m.csd[33] = 1;
    const bool wide = scenario == "wide" || scenario == "wide-fallback" || scenario == "wide-restore-error";
    if (wide) m.put(0x40, 1u << 18, 4);
    if (scenario == "wide-fallback" || scenario == "wide-restore-error") m.corrupt_wide = true;
    if (scenario == "wide-restore-error") m.fail_width_restore = true;
    const auto init = driver.initialize(200000000, true, wide ? 8 : 1);
    if (scenario == "wide-restore-error") { assert(init != Error::Ok && !driver.ready()); return 0; }
    if (scenario == "clock-timeout" || scenario == "reset-timeout" || scenario == "command-timeout") {
        assert(init == Error::Timeout && !driver.ready()); assert(m.time < 4000000); return 0;
    }
    if (scenario == "byte-addressing" || scenario == "native-4k") { assert(init == Error::Unsupported); return 0; }
    assert(init == Error::Ok && driver.ready() && driver.info().sectors == 128);
    if (scenario.starts_with("dma")) {
        const DmaArena arena{m.dma.data(), Model::dma_phys, m.dma.size()};
        assert(driver.enable_dma(arena) == Error::Unsupported && !driver.dma_enabled());
        m.put(0x40, 1u << 19, 4);
        for (auto invalid : {DmaArena{}, DmaArena{arena.cpu+1,arena.physical,arena.size},
                DmaArena{arena.cpu,arena.physical+1,arena.size}, DmaArena{arena.cpu,arena.physical,arena.size-1},
                DmaArena{arena.cpu,1ull<<32,arena.size},DmaArena{arena.cpu,0xfffff000,arena.size}})
            assert(driver.enable_dma(invalid) == Error::Invalid && !driver.dma_enabled());
        assert(driver.enable_dma(arena) == Error::Ok && driver.dma_enabled());
        const auto fifo = m.fifo_words;
        std::array<u8, 65537> buffer{};
        if (scenario == "dma") {
            for (auto count : {1u, 8u, 64u, 65u, 128u}) {
                assert(driver.read(0, count, buffer.data()+1) == Error::Ok);
                assert(!std::memcmp(buffer.data()+1,m.disk.data(),count*512));
                for (u32 i=1;i<=count*512;++i) buffer[i]=u8(i+count);
                assert(driver.write_buffered(0,count,buffer.data()+1)==Error::Ok);
                assert(!std::memcmp(buffer.data()+1,m.disk.data(),count*512));
            }
            assert(m.dma_transfers==10 && m.fifo_words==fifo);
            // RPMB continues through the PIO transport with DMA configured.
            auto request=frame(2);Frame response{};
            assert(driver.rpmb(&request,1,&response,1)==Error::Ok && response.bytes[484]==0x76);
            assert(m.dma_transfers==10 && m.fifo_words>fifo);
        } else {
            if (scenario == "dma-timeout" || scenario == "dma-reset-error") m.data_stall=true;
            if (scenario == "dma-error") m.adma_error=true;
            if (scenario == "dma-reset-error") m.reset_stall=true;
            buffer.fill(0x39);
            const auto result=driver.read(0,128,buffer.data()+1);
            assert(result == (scenario == "dma-error" ? Error::Controller : Error::Timeout));
            for (auto byte : buffer) assert(byte==0x39); // never publish partial DMA reads
            assert(m.dma_transfers==1 && m.fifo_words==fifo);
            if (scenario=="dma-reset-error") assert(!driver.ready());
            else { m.data_stall=false;m.adma_error=false;assert(driver.read(0,1,buffer.data())==Error::Ok); }
        }
        return 0;
    }
    if (wide) {
        assert(driver.info().bus_width == (scenario == "wide" ? 8u : 1u));
        assert(bool(m.get(0x28, 1) & 0x20) == (scenario == "wide"));
        std::array<u8, 512> block{};
        assert(driver.read(0, 1, block.data()) == Error::Ok);
        assert(std::memcmp(block.data(), m.disk.data(), 512) == 0);
        return 0;
    }
    assert(m.get(0x3e, 2) == 8); // inherited 1.8V signaling preserved
    // Backward-compatible timing: 200 MHz / 8 = 25 MHz. The divisor is the
    // ceiling-rounded even value for the 26 MHz target, encoded as divisor/2 in
    // the high byte, so 26 MHz lands on 0x04. This used to assert 0x0800 (12.5
    // MHz) and was left behind when the bus moved off that clock.
    assert((m.get(0x2c, 2) & 0xff00) == 0x0400); // 200MHz / 8 = 25MHz
    m.commands.clear();
    Frame request = frame(2), response{};
    if (scenario == "buffered-flush") {
        std::array<u8, 512> buffer{};
        assert(driver.write_buffered(0, 1, buffer.data()) == Error::Ok);
        for (const auto& c : m.commands) assert(c.index != 6);
        assert(driver.flush() == Error::Ok);
        unsigned flushes = 0;
        for (const auto& c : m.commands) if (c.index == 6 && ((c.arg >> 16) & 255) == 32) ++flushes;
        assert(flushes == 1);
        m.fail_command = 6;
        assert(driver.flush() != Error::Ok);
    } else if (scenario == "user128") {
        std::array<u8, 65536> buffer{};
        assert(driver.read(0, 128, buffer.data()) == Error::Ok);
        assert(std::memcmp(buffer.data(), m.disk.data(), buffer.size()) == 0);
        buffer.fill(0x6e);
        assert(driver.write_buffered(0, 128, buffer.data()) == Error::Ok);
        assert(driver.flush() == Error::Ok);
        assert(std::memcmp(buffer.data(), m.disk.data(), buffer.size()) == 0);
        assert(driver.read(0, 129, buffer.data()) == Error::Invalid);
    } else if (scenario == "blocks" || scenario == "cache-flush") {
        std::array<u8, 1025> buffer{};
        assert(driver.read(3, 2, buffer.data() + 1) == Error::Ok);
        assert(std::memcmp(buffer.data() + 1, m.disk.data() + 3 * 512, 1024) == 0);
        std::memset(buffer.data() + 1, 0xa6, 1024);
        assert(driver.write(3, 2, buffer.data() + 1) == Error::Ok);
        assert(std::memcmp(buffer.data() + 1, m.disk.data() + 3 * 512, 1024) == 0);
        if (scenario == "cache-flush") {
            bool flushed = false;
            for (const auto& c : m.commands)
                if (c.index == 6 && ((c.arg >> 16) & 255) == 32) flushed = true;
            assert(flushed);
        }
        const auto n = m.commands.size();
        assert(driver.read(127, 2, buffer.data()) == Error::Invalid);
        assert(driver.read(~0u, 2, buffer.data()) == Error::Invalid);
        assert(driver.write(0, 0, buffer.data()) == Error::Invalid);
        assert(driver.read(0, 1, nullptr) == Error::Invalid);
        assert(m.commands.size() == n);
    } else if (scenario == "counter" || scenario == "card-result") {
        if (scenario == "card-result") m.card_result = 7;
        assert(driver.rpmb(&request, 1, &response, 1) == Error::Ok);
        assert(response.bytes[484] == 0x76 && response.bytes[510] == 2);
        assert(response.bytes[509] == m.card_result && m.partition == 0x48);
        std::vector<u32> sequence;
        for (const auto& c : m.commands) if (c.index != 13) sequence.push_back(c.index);
        assert((sequence == std::vector<u32>{6, 23, 25, 23, 18, 6}));
    } else if (scenario == "write" || scenario == "legacy-reliable") {
        std::array<Frame, 32> requests;
        for (auto& f : requests) f = frame(0x1003, 16, 32);
        if (scenario == "legacy-reliable") {
            m.csd[166] = 0;
            assert(driver.initialize(200000000) == Error::Ok);
            assert(driver.rpmb(requests.data(), 32, &response, 1) == Error::Unsupported);
            for (auto& f : requests) f = frame(3, 16, 2);
        }
        assert(driver.rpmb(requests.data(), scenario == "write" ? 32 : 2, &response, 1) == Error::Ok);
        assert(m.partition == 0x48);
        bool saw = false;
        for (const auto& c : m.commands) if (c.index == 23 && (c.arg >> 31)) saw = true;
        assert(saw);
    } else if (scenario == "read32") {
        request = frame(4, 12);
        std::array<Frame, 32> responses;
        assert(driver.rpmb(&request, 1, responses.data(), 32) == Error::Ok);
        for (auto& f : responses) assert(f.bytes[510] == 4 && f.bytes[484] == 0x76);
    } else if (scenario == "invalid") {
        assert(driver.rpmb(&request, 0, &response, 1) == Error::Invalid);
        request = frame(1);
        request.bytes[196] = 0x5a; // key material is opaque to the transport
        assert(driver.rpmb(&request, 1, &response, 1) == Error::Ok);
        assert(response.bytes[510] == 1 && response.bytes[511] == 0 &&
               response.bytes[508] == 0 && response.bytes[509] == 0);
        std::vector<u32> key_sequence;
        for (const auto& c : m.commands) if (c.index != 13) key_sequence.push_back(c.index);
        assert((key_sequence == std::vector<u32>{6, 23, 25, 23, 25, 23, 18, 6}));
        m.commands.clear();
        request = frame(4, 0xffff);
        assert(driver.rpmb(&request, 1, &response, 1) == Error::Invalid);
        request = frame(3, 2, 2);
        assert(driver.rpmb(&request, 1, &response, 1) == Error::Invalid);
        assert(m.commands.empty());
    } else if (scenario == "data-timeout" || scenario == "crc-error" || scenario == "r1-error" ||
               scenario == "restore-error" || scenario == "switch-error" || scenario == "bad-response") {
        if (scenario == "data-timeout") m.data_stall = true;
        if (scenario == "crc-error") m.error_with_ready = true;
        if (scenario == "r1-error") m.fail_r1 = 23;
        if (scenario == "switch-error") m.fail_command = 6;
        if (scenario == "restore-error") m.fail_restore = true;
        if (scenario == "bad-response") m.corrupt_response = true;
        const auto result = driver.rpmb(&request, 1, &response, 1);
        if (scenario == "bad-response") {
            assert(result == Error::Ok && response.bytes[510] == 1 && m.partition == 0x48);
            return 0;
        }
        assert(result != Error::Ok);
        if (scenario == "restore-error") {
            assert(result == Error::RestoreFailed && !driver.ready());
            assert(driver.write(0, 1, &response) == Error::NotReady);
        } else assert(m.partition == 0x48);
        if (scenario == "data-timeout") assert(result == Error::Timeout && m.time < 4000000);
        if (scenario == "crc-error") assert(result == Error::Controller && driver.failure().command == 25);
        if (scenario == "r1-error") assert(result == Error::Card);
    } else if (scenario == "key-missing" || scenario == "key-unknown" || scenario == "provision" ||
               scenario == "wrong-counter") {
        anne::emmc::Backend backend(driver);
        std::array<u8, 0x8000> window{};
        if (scenario == "key-missing") m.card_result = 7;
        if (scenario == "key-unknown") m.corrupt_response = true;
        const auto key = backend.key_info();
        if (scenario == "key-missing" || scenario == "key-unknown") {
            assert(!key.present() && key.missing() == (scenario == "key-missing"));
            le32(window.data() + 0x6a14, 2);
            le32(window.data() + 0x6a0c, 32);
            le32(window.data() + 0x6a1c, 32);
            const auto n = m.commands.size();
            assert(backend.service(window.data(), window.size()) == 7);
            assert(m.commands.size() == n); // no large reads on an unkeyed card
        } else if (scenario == "provision") {
            assert(key.present());
            window[511] = 2;
            const auto n = m.commands.size();
            assert(backend.provision_key(window.data(), window.size()) == 0xff09);
            assert(m.commands.size() == n);
            window[511] = 1; window[196] = 0xa4;
            assert(backend.provision_key(window.data(), window.size()) == 0);
            assert(window[510] == 1 && !window[511] && m.partition == 0x48);
        } else {
            assert(key.present());
            m.corrupt_response = true;
            le32(window.data() + 0x6a14, 4);
            le32(window.data() + 0x6a0c, 2);
            window[511] = 2;
            assert(backend.service(window.data(), window.size()) == 0);
            assert(window[510] == 1 && !window[511] && !backend.write_pending());
        }
    } else if (scenario == "bridge") {
        anne::emmc::Backend backend(driver);
        anne::rpmb::ConfigInfo config;
        assert(backend.config(config) && config.total_blks == 16384);
        assert(backend.key_info().present());
        std::array<u8, 0x8000> window{};
        auto setup = [&](u32 state, u32 offset, u32 count) {
            le32(window.data() + 0x6a00, 1);
            le32(window.data() + 0x6a08, 8);
            le32(window.data() + 0x6a0c, 2);
            le32(window.data() + 0x6a14, state);
            le32(window.data() + 0x6a18, offset);
            le32(window.data() + 0x6a1c, count);
            Frame f = frame(state == 5 ? 3 : state == 2 ? 4 : 2, u16(8 + offset), u16(count));
            std::memcpy(window.data(), &f, 512);
        };
        setup(4, 0, 1);
        assert(backend.service(window.data(), window.size()) == 0 && backend.write_pending());
        setup(3, 0, 1);
        assert(backend.service(window.data(), window.size()) != 0 && backend.write_pending());
        setup(5, 1, 1);
        assert(backend.service(window.data(), window.size()) != 0 && backend.write_pending());
        setup(5, 0, 1);
        assert(backend.service(window.data(), window.size()) == 0 && backend.write_pending());
        setup(5, 1, 1);
        assert(backend.service(window.data(), window.size()) == 0 && !backend.write_pending());
        assert(window[484] == 0x76 && window[510] == 3);
        setup(2, 0, 2);
        assert(backend.service(window.data(), window.size()) == 0 && window[512 + 510] == 4);
        setup(1, 0, 1);
        assert(backend.service(window.data(), window.size()) == 0xff09);
        assert(backend.service(window.data(), 512) == 0xff03);
        assert(backend.service(nullptr, window.size()) == 0xff03);
    } else assert(false);
}
