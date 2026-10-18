"""Execute hash-pinned Huawei OSS routines, rather than recreating their plans."""
from pathlib import Path
import hashlib
import os
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
REVISION = '65e545cbaa11d132e243287b150146bc2a51c45e'


def pinned(name, digest):
    data = (Path(os.environ['AME_HI6250_LINUX_ORACLE']) / name).read_bytes()
    if hashlib.sha256(data).hexdigest() != digest:
        raise AssertionError(f'{name} must match Huawei OSS {REVISION}')
    return data.decode()


@unittest.skipUnless(shutil.which('clang++') and os.environ.get('AME_HI6250_LINUX_ORACLE'),
                     'requires clang++ and AME_HI6250_LINUX_ORACLE')
class HuaweiRpmbOracle(unittest.TestCase):
    def test_real_command_builders_and_key_store(self):
        source = pinned('hisi_rpmb.c',
                        'd8b26ecc8a8534ecb112aaf51b9d184067e1ea3294076f620cae0b9d50e65ade')
        header = pinned('hisi_rpmb.h',
                        '85435c172a33ff32eea2d3c95d1bcab80db639c14f7422a10da84041671e53b8')
        header = '\n'.join(line for line in header.splitlines() if not line.startswith('#include'))
        builders = source[source.index('static inline void mmc_rpmb_combine_cmd('):
                          source.index('int ufs_bsg_ioctl_rpmb_cmd(')]
        key_store = source[source.index('static ssize_t mmc_rpmb_key_store('):
                           source.index('static ssize_t ufs_rpmb_key_store(')]
        api = pinned('include_linux_mmc_rpmb.h',
                     '9ca18286c3286127571ea485ea33b960f05c05a2518b6a3c999e46dce7f9134f')
        func_id = re.search(r'enum func_id\s*\{[^}]+\};', api).group()
        harness = r'''
#include <cstdlib>
#include <cstring>
#include <string>
#include "emmc_model.h"
#include "emmc.h"
#include "rpmb_provisioning.h"
namespace oracle {
using u64 = uint64_t;
using u32 = uint32_t;
constexpr unsigned MMC_WRITE_MULTIPLE_BLOCK = 25, MMC_READ_MULTIPLE_BLOCK = 18;
constexpr unsigned MMC_RSP_SPI_R1 = 1 << 7, MMC_RSP_R1 = 21, MMC_CMD_ADTC = 1 << 5;
inline uint16_t swap16(uint16_t v) { return uint16_t((v << 8) | (v >> 8)); }
#define be16_to_cpu(v) swap16(v)
#define cpu_to_be16(v) swap16(v)
struct mmc_ioc_cmd { int write_flag; unsigned opcode, flags, arg, blksz, blocks; };
struct mmc_blk_ioc_data { mmc_ioc_cmd ic; void* buf; u64 buf_bytes; };
struct mmc_blk_ioc_rpmb_data { mmc_blk_ioc_data data[3]; };
''' + header + '\n' + func_id + '\n' + builders + r'''
struct device {};
struct device_attribute {};
struct block_device {};
struct { rpmb_request* rpmb_request; block_device* blkdev; device* dev; } hisi_rpmb;
static unsigned ready_after, delays, smcs, programs;
static int smc_result;
static unsigned card_result;
constexpr int GFP_KERNEL = 0, O_RDWR = 2, O_NDELAY = 4;
constexpr const char* EMMC_RPMB_BLOCK_DEVICE_NAME = "oracle";
mmc_blk_ioc_rpmb_data* kzalloc(size_t size, int) {
    return static_cast<mmc_blk_ioc_rpmb_data*>(std::calloc(1, size));
}
void kfree(void* p) { std::free(p); }
block_device* blkdev_get_by_path(const char*, int, device*) { static block_device b; return &b; }
void blkdev_put(block_device*, int) {}
#define IS_ERR(p) false
#define dev_err(...) ((void)0)
#define pr_err(...) ((void)0)
void print_frame_buf(const char*, void*, int, int) {}
void generate_key(rpmb_request& request) {
    auto* bytes = reinterpret_cast<unsigned char*>(&request.frame[0]);
    bytes[511] = 1;
    for (unsigned i = 196; i < 228; ++i) bytes[i] = uint8_t(i ^ 0x5a);
    request.key_frame_status = 1;
}
int atfd_hisi_rpmb_smc(u64 fid, u64 x1, u64 x2, u64 x3) {
    assert(fid == RPMB_SVC_SET_KEY && !x1 && !x2 && !x3);
    ++smcs;
    assert(!hisi_rpmb.rpmb_request->key_frame_status);
    for (auto byte : reinterpret_cast<rpmb_frame*>(hisi_rpmb.rpmb_request)->mac) assert(byte == 0);
    if (!smc_result && !ready_after) generate_key(*hisi_rpmb.rpmb_request);
    return smc_result;
}
void mdelay(unsigned ms) {
    assert(ms == 5);
    if (++delays == ready_after) generate_key(*hisi_rpmb.rpmb_request);
}
int mmc_blk_ioctl_rpmb_cmd(unsigned, block_device*, mmc_blk_ioc_rpmb_data* plan) {
    ++programs;
    assert(plan->data[0].ic.opcode == 25 && unsigned(plan->data[0].ic.write_flag) == 0x80000001);
    assert(plan->data[1].ic.opcode == 25 && plan->data[1].ic.write_flag == 1);
    assert(plan->data[2].ic.opcode == 18 && plan->data[2].ic.write_flag == 0);
    auto& frame = *static_cast<rpmb_frame*>(plan->data[2].buf);
    frame = {};
    frame.request = cpu_to_be16(RPMB_RESP_KEY);
    frame.result = cpu_to_be16(card_result);
    return 0;
}
''' + key_store + r'''
}

static void set16(u8* p, u16 v) { p[0] = u8(v >> 8); p[1] = u8(v); }
static void compare_plans() {
    for (unsigned operation : {1u, 2u, 3u, 4u}) {
        for (unsigned region : {0u, 0x1000u}) {
            for (unsigned n = 1; n <= 32; ++n) {
                if ((operation == 1 || operation == 2) && n != 1) continue;
                if (operation == 3 && n != 1 && n != 2 && n != 32) continue;
                Model model;
                Driver driver(model.io());
                assert(driver.initialize(200000000) == Error::Ok);
                model.transfers.clear();
                std::array<Frame, 32> frames{}, response{};
                for (auto& frame : frames) {
                    for (unsigned i = 0; i < 512; ++i) frame.bytes[i] = u8(i * 13);
                    set16(frame.bytes + 510, u16(region | operation));
                    set16(frame.bytes + 504, 8);
                    set16(frame.bytes + 506, u16(n));
                }
                oracle::rpmb_request request{};
                std::memcpy(request.frame, frames.data(), sizeof(request.frame));
                request.info.current_rqst.blks = n;
                oracle::mmc_blk_ioc_rpmb_data plan{};
                if (operation == 1) oracle::mmc_rpmb_set_key(&request, &plan);
                if (operation == 2) oracle::mmc_rpmb_get_counter(&request, &plan);
                if (operation == 3) oracle::mmc_rpmb_write_data(&request, &plan);
                if (operation == 4) oracle::mmc_rpmb_read_data(&request, &plan);
                assert(driver.rpmb(frames.data(), operation == 3 ? n : 1,
                                   response.data(), operation == 4 ? n : 1) == Error::Ok);
                const unsigned commands = operation == 1 || operation == 3 ? 3 : 2;
                assert(model.transfers.size() == commands);
                for (unsigned i = 0; i < commands; ++i) {
                    const auto& actual = model.transfers[i];
                    const auto& expected = plan.data[i];
                    assert(actual.opcode == expected.ic.opcode && actual.count == expected.ic.blocks);
                    assert(actual.reliable == bool(unsigned(expected.ic.write_flag) >> 31));
                    if (expected.ic.write_flag)
                        assert(actual.bytes.size() == expected.buf_bytes &&
                               std::memcmp(actual.bytes.data(), expected.buf, expected.buf_bytes) == 0);
                }
            }
        }
    }
}
struct Fixture {
    Model model;
    Driver driver{model.io()};
    anne::emmc::Backend backend{driver};
    std::array<u8, 0x8000> window{};
    unsigned polls{}, smcs{}, programs{};
    Fixture() { assert(driver.initialize(200000000) == Error::Ok); model.card_result = 7; }
};
static Fixture* current;
static u64 program(volatile u8* window) {
    ++current->programs;
    current->model.card_result = oracle::card_result;
    return current->backend.provision_key(window, current->window.size());
}
static anne::rpmb::KeyInfo key_info() { return current->backend.key_info(); }
static void make_key() {
    auto& w = current->window;
    w[511] = 1;
    for (unsigned i = 196; i < 228; ++i) w[i] = u8(i ^ 0x5a);
    w[anne::rpmb::kKeyStatusOffset] = 1;
}
static u64 firmware(u64 fid, u64 x1, u64 x2, u64 x3) {
    assert(fid == anne::rpmb::kSetKeySmc && !x1 && !x2 && !x3);
    ++current->smcs;
    for (unsigned i = 0; i < 512; ++i) assert(current->window[i] == 0);
    assert(!current->window[anne::rpmb::kKeyStatusOffset]);
    if (!oracle::smc_result && !oracle::ready_after) make_key();
    return u64(oracle::smc_result);
}
static void delay(u32 ms) {
    assert(ms == 5);
    if (++current->polls == oracle::ready_after) make_key();
}
static void compare_key_store() {
    for (unsigned ready_after : {0u, 1u, 20u, 299u, 300u, 301u}) {
        for (int smc_result : {0, -1}) for (unsigned card_result : {0u, 1u}) {
            oracle::ready_after = ready_after; oracle::smc_result = smc_result;
            oracle::card_result = card_result;
            oracle::delays = oracle::smcs = oracle::programs = 0;
            oracle::rpmb_request request{};
            request.key_frame_status = 1; // deliberately stale
            std::memset(request.frame[0].mac, 0xa5, sizeof(request.frame[0].mac));
            oracle::hisi_rpmb.rpmb_request = &request;
            const bool expected = oracle::mmc_rpmb_key_store(nullptr, nullptr, "set_key", 7) == 7;
            Fixture fixture;
            current = &fixture;
            fixture.window[anne::rpmb::kKeyStatusOffset] = 1;
            fixture.window[196] = 0xa5;
            anne::rpmb_diag::Storage storage{};
            storage.provision_key = program; storage.key_info = key_info; storage.delay_ms = delay;
            const auto actual = anne::rpmb_diag::provision(fixture.window.data(), storage, firmware);
            assert((actual.result == MX_RPMB_OK) == expected);
            assert(fixture.smcs == oracle::smcs && fixture.polls == oracle::delays && fixture.programs == oracle::programs);
            for (unsigned i = 196; i < 228; ++i) assert(fixture.window[i] == 0);
        }
    }
}
int main() {
    (void)oracle::rpmb_err_msg;
    compare_plans();
    compare_key_store();
}
'''
        with tempfile.TemporaryDirectory(prefix='ame-huawei-rpmb-') as directory:
            cpp, probe = Path(directory) / 'oracle.cc', Path(directory) / 'oracle'
            cpp.write_text(harness)
            build = subprocess.run([
                'clang++', '-std=c++23', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-parameter', '-Wno-unused-function', '-DANKO_HOST_TEST=1',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                *['-I' + str(ROOT / p) for p in ('kernel/include', 'shared/include',
                                                'boards/huawei-anne', 'tests')],
                str(cpp), str(ROOT / 'shared/drivers/emmc/sdhci.cc'),
                str(ROOT / 'boards/huawei-anne/emmc.cc'),
                str(ROOT / 'boards/huawei-anne/rpmb_provisioning.cc'), '-o', str(probe)],
                capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(probe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stderr)


if __name__ == '__main__':
    unittest.main()
