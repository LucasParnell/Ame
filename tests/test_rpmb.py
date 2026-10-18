from pathlib import Path
import hashlib
import os
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
ORACLE_REVISION = '65e545cbaa11d132e243287b150146bc2a51c45e'


def linux_oracle(name, digest):
    data = (Path(os.environ['AME_HI6250_LINUX_ORACLE']) / name).read_bytes()
    if hashlib.sha256(data).hexdigest() != digest:
        raise AssertionError(f'{name} must match Hi6250 Linux revision {ORACLE_REVISION}')
    return data.decode()


@unittest.skipUnless(shutil.which('clang++'), 'requires clang++')
class Rpmb(unittest.TestCase):
    def test_firmware_owned_key_provisioning(self):
        with tempfile.TemporaryDirectory(prefix='ame-rpmb-key-') as directory:
            probe = Path(directory) / 'key'
            build = subprocess.run([
                'clang++', '-std=c++23', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                '-DANKO_HOST_TEST=1', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-I' + str(ROOT / 'kernel/include'), '-I' + str(ROOT / 'shared/include'),
                '-I' + str(ROOT / 'boards/huawei-anne'),
                str(ROOT / 'tests/rpmb_provision_probe.cc'),
                str(ROOT / 'boards/huawei-anne/rpmb_diagnostics.cc'),
                str(ROOT / 'boards/huawei-anne/rpmb_provisioning.cc'), '-o', str(probe)],
                capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            for scenario in ('delayed', 'immediate', 'already', 'key-status', 'prepare-failed', 'stale-request',
                             'registration-failed', 'query-error', 'wrong-counter-type', 'smc-failed',
                             'timeout', 'wrong-frame', 'card-error', 'transport-error', 'verify-failed'):
                with self.subTest(scenario=scenario):
                    run = subprocess.run([str(probe), scenario], capture_output=True, text=True, timeout=10)
                    self.assertEqual(run.returncode, 0, run.stderr)

    def test_storage_completion_and_successive_notifications(self):
        with tempfile.TemporaryDirectory(prefix='ame-rpmb-storage-') as directory:
            probe = Path(directory) / 'storage'
            build = subprocess.run([
                'clang++', '-std=c++23', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                '-DANKO_HOST_TEST=1', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-I' + str(ROOT / 'kernel/include'), '-I' + str(ROOT / 'shared/include'),
                '-I' + str(ROOT / 'boards/huawei-anne'),
                str(ROOT / 'tests/rpmb_storage_probe.cc'),
                str(ROOT / 'boards/huawei-anne/rpmb_diagnostics.cc'),
                str(ROOT / 'boards/huawei-anne/rpmb_provisioning.cc'), '-o', str(probe)],
                capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            for scenario in ('prepare-failed', 'two-requests', 'notify-during-ff06',
                             'duplicate', 'service-error', 'completion-failed', 'write-lease'):
                with self.subTest(scenario=scenario):
                    run = subprocess.run([str(probe), scenario], capture_output=True, text=True, timeout=10)
                    self.assertEqual(run.returncode, 0, run.stderr)

    def test_gic_notification_routing_and_eoi(self):
        with tempfile.TemporaryDirectory(prefix='ame-rpmb-gic-') as directory:
            for board in ('native', 'synthetic', 'generic'):
                with self.subTest(board=board):
                    probe = Path(directory) / board
                    defines = ['-DANKO_HOST_TEST=1']
                    if board != 'generic':
                        defines += ['-DANKO_BOARD_HUAWEI_ANNE=1']
                    if board == 'synthetic':
                        defines += ['-DANKO_ANNE_DIAG_TEST=1']
                    result = subprocess.run([
                        'clang++', '-std=c++23', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        *defines, '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        '-I' + str(ROOT / 'kernel/include'), '-I' + str(ROOT / 'shared/include'),
                        '-I' + str(ROOT / 'boards/huawei-anne'),
                        str(ROOT / 'tests/rpmb_gic_probe.cc'),
                        str(ROOT / 'boards/huawei-anne/rpmb_diagnostics.cc'),
                str(ROOT / 'boards/huawei-anne/rpmb_provisioning.cc'), '-o', str(probe)],
                        capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    result = subprocess.run([str(probe)], capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stderr)

    def test_notification_backed_error_completion(self):
        with tempfile.TemporaryDirectory(prefix='ame-rpmb-lifecycle-') as directory:
            probe = Path(directory) / 'lifecycle'
            result = subprocess.run([
                'clang++', '-std=c++23', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                '-DANKO_HOST_TEST=1', '-pthread', '-fsanitize=address,undefined',
                '-fno-omit-frame-pointer',
                '-I' + str(ROOT / 'kernel/include'), '-I' + str(ROOT / 'shared/include'),
                '-I' + str(ROOT / 'boards/huawei-anne'),
                str(ROOT / 'tests/rpmb_lifecycle_probe.cc'),
                str(ROOT / 'boards/huawei-anne/rpmb_diagnostics.cc'),
                str(ROOT / 'boards/huawei-anne/rpmb_provisioning.cc'), '-o', str(probe)],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            for scenario in ('busy-research', 'stale', 'registration-failed', 'no-event',
                             'old-event', 'disabled-trigger', 'concurrent', 'notify-before-counter',
                             'duplicate', 'completion-failed'):
                with self.subTest(scenario=scenario):
                    result = subprocess.run([str(probe), scenario], capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stderr)

    def test_linux_config_wire_bytes_and_shared_window_bounds(self):
        with tempfile.TemporaryDirectory(prefix='ame-rpmb-') as directory:
            probe = Path(directory) / 'probe'
            result = subprocess.run([
                'clang++', '-std=c++23', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-I' + str(ROOT / 'kernel/include'),
                '-I' + str(ROOT / 'boards/huawei-anne'),
                str(ROOT / 'tests/rpmb_probe.cc'), '-o', str(probe)],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(probe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)

    @unittest.skipUnless(os.environ.get('AME_HI6250_LINUX_ORACLE'),
                         'set AME_HI6250_LINUX_ORACLE to the pinned Linux source directory')
    def test_against_linux_emmc_get_rpmb_info(self):
        source = Path(os.environ['AME_HI6250_LINUX_ORACLE'])
        files = {}
        for name, digest in {
            'emmc-rpmb.c': 'df56965d38580067c4863ac9921e6ed17ed02e81abaa9d4b911daf7f77399d08',
            'bootdevice.h': 'ba20eae7c010b95cd22a3e7757796d69d5c5b0c69e6ae7b6b0e121907ccd8e53',
        }.items():
            files[name] = (source / name).read_bytes()
            self.assertEqual(hashlib.sha256(files[name]).hexdigest(), digest,
                             'oracle must match Hi6250 Linux revision 65e545cbaa11d132e243287b150146bc2a51c45e')
        # Compile the real Linux calculation and wire structure with tiny
        # bootdevice/card shims; no vendored Linux implementation or network.
        structure = files['bootdevice.h'].decode().split('struct rpmb_config_info{', 1)[1].split('};', 1)[0]
        routine = files['emmc-rpmb.c'].decode().split('void emmc_get_rpmb_info', 1)[1]
        header = linux_oracle('emmc-rpmb.h',
                              'c2ad6b6d439aced6ea62a85b271a1c218cfc5bbbe594b61cad9c698b327d5877')
        header = '\n'.join(line for line in header.splitlines()
                           if not line.startswith('#include'))
        fields = ('total_blks', 'blk_size', 'unit_size', 'region_enable', 'read_align',
                  'read_frame_support', 'write_align', 'write_frame_support')
        setters = '\n'.join(
            f'void set_rpmb_{field}(u64 v) {{ observed.rpmb_{field} = v; }}' for field in fields)
        harness = r'''
#include <cassert>
#include <cstring>
#include "rpmb.h"
''' + header + r'''
enum { EXT_CSD_RPMB_MULT, EXT_CSD_WR_REL_PARAM, EXT_CSD_VENDOR_FEATURE_SUPPORT,
       EXT_CSD_RPMB_REGION1_SIZE_MULT, EXT_CSD_RPMB_REGION2_SIZE_MULT,
       EXT_CSD_RPMB_REGION3_SIZE_MULT, EXT_CSD_RPMB_REGION4_SIZE_MULT };
struct mmc_card { struct {
    u8 raw_rpmb_size_mult, raw_wr_rel_param, raw_vendor_feature_support;
    u8 raw_rpmb_region1_size_mult, raw_rpmb_region2_size_mult;
    u8 raw_rpmb_region3_size_mult, raw_rpmb_region4_size_mult;
} ext_csd; };
''' + 'struct rpmb_config_info{' + structure + '};\n' + r'''
rpmb_config_info observed{};
void set_rpmb_config_ready_status() {}
void set_rpmb_region_size(int i, u8 value) { observed.rpmb_region_size[i] = value; }
''' + setters + '\nvoid emmc_get_rpmb_info' + routine + r'''
int main() {
    static_assert(sizeof(observed) == sizeof(anne::rpmb::ConfigInfo));
    for (unsigned size = 0; size < 256; ++size)
        for (unsigned rel = 0; rel < 256; ++rel)
            for (unsigned vendor : {0, 1, 254, 255}) {
                u8 csd[7] = {u8(size), u8(rel), u8(vendor), 0, 0, 0, 0};
                for (unsigned regions = 0; regions < 3; ++regions) {
                    csd[3] = regions == 1 ? 8 : 0;
                    csd[4] = regions == 2 ? 1 : 0;
                    mmc_card card{};
                    emmc_get_rpmb_info(&card, csd);
                    auto actual = anne::rpmb::emmc_config(
                        {csd[0], csd[1], csd[2], {csd[3], csd[4], csd[5], csd[6]}});
                    assert(std::memcmp(&actual, &observed, sizeof(actual)) == 0);
                }
            }
}
'''
        # Range-for over a braced list needs the host initializer_list header.
        harness = '#include <initializer_list>\n' + harness
        with tempfile.TemporaryDirectory(prefix='ame-rpmb-oracle-') as directory:
            cpp, probe = Path(directory) / 'oracle.cc', Path(directory) / 'oracle'
            cpp.write_text(harness)
            result = subprocess.run([
                'clang++', '-std=c++23', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                '-Wno-missing-field-initializers',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-I' + str(ROOT / 'kernel/include'), '-I' + str(ROOT / 'boards/huawei-anne'),
                str(cpp), '-o', str(probe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(probe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)

    @unittest.skipUnless(os.environ.get('AME_HI6250_LINUX_ORACLE'),
                         'set AME_HI6250_LINUX_ORACLE to the pinned Linux source directory')
    def test_linux_service_ids_memory_map_and_request_layout(self):
        header = linux_oracle('hisi_rpmb.h',
                              '85435c172a33ff32eea2d3c95d1bcab80db639c14f7422a10da84041671e53b8')
        memory = linux_oracle('global_ddr_map.h',
                              '653d6f0898103646057fe5eb0c817baa73facdc493b5249313bd7016ed59377b')
        share = linux_oracle('share.dtsi',
                             '9dc4f50b3386c2ec628fa9167f2cda3a4229149b26f886138af00b6202add43b')
        config = linux_oracle('merge_hi6250_defconfig',
                              '10921aba1843762a557f46538ee5048a451dd1140d60b96a620362374bb7ff14')
        bootdevice = linux_oracle('bootdevice.h',
                                  'ba20eae7c010b95cd22a3e7757796d69d5c5b0c69e6ae7b6b0e121907ccd8e53')
        bootdevice_enum = re.search(r'enum bootdevice_type\s*\{[^}]+\};', bootdevice).group()
        smp = linux_oracle('arch_arm64_kernel_smp.c',
                           '0f15705fb38ffd3ce1ce3a11e8179f654506d8d0e9fe4772abc67f3f89cfefb9')
        ipi_enum = re.search(r'enum ipi_msg_type\s*\{[^}]+\};', smp).group()
        # The configuration oracle above deliberately excludes this optional
        # device-tree override, as does the pinned platform's base defconfig.
        self.assertNotIn('CONFIG_HISI_MMC_RPMB_AUTO_RESIZE=y', config)
        rpmb_node = share.split('compatible = "hisilicon,hisi-rpmb";', 1)[1].split('};', 1)[0]
        offset, size = re.search(r'hisi,bl31-share-mem\s*=\s*<(0x\w+)\s+(0x\w+)>',
                                 rpmb_node).groups()
        # Compile the actual Linux definitions, omitting kernel-only includes.
        header = '\n'.join(line for line in header.splitlines()
                           if not line.startswith('#include'))
        harness = '#include "rpmb.h"\n#include "boot_diagnostics.h"\n' + memory + header + '\n' + bootdevice_enum + ipi_enum + f'''
static_assert(anne::rpmb::kCallCountSmc == RPMB_SVC_CALL_COUNT);
static_assert(anne::rpmb::kUidSmc == RPMB_SVC_UID);
static_assert(anne::rpmb::kVersionSmc == RPMB_SVC_VERSION);
static_assert(anne::rpmb::kRequestAddressSmc == RPMB_SVC_REQUEST_ADDR);
static_assert(anne::rpmb::kRequestDoneSmc == RPMB_SVC_REQUEST_DONE);
static_assert(anne::rpmb::kSetKeySmc == RPMB_SVC_SET_KEY);
static_assert(anne::rpmb::kErrorMmc == RPMB_ERR_MMC_ERR);
static_assert(anne::rpmb::kErrorKey == RPMB_ERR_KEY);
static_assert(anne::rpmb::kErrorSetKey == RPMB_ERR_SET_KEY);
static_assert(anne::rpmb::kErrorCounter == RPMB_ERR_GET_COUNT);
static_assert(anne::rpmb::kErrorTimeout == RPMB_ERR_TIMEOUT);
static_assert(anne::rpmb::kCounterSmc == RPMB_SVC_COUNTER);
static_assert(anne::rpmb::kErrorBlockDevice == RPMB_ERR_BLKDEV);
static_assert(anne::rpmb::kRequestSgi == IPI_SECURE_RPMB);
static_assert(anne::rpmb::kBootDeviceEmmc == BOOT_DEVICE_EMMC);
static_assert(anne::kRpmbSharedBase == HISI_SUB_RESERVED_BL31_SHARE_MEM_PHYMEM_BASE);
static_assert(anne::kRpmbSharedSize == HISI_SUB_RESERVED_BL31_SHARE_MEM_PHYMEM_SIZE);
static_assert(anne::kRpmbRequestAddress == anne::kRpmbSharedBase + {offset});
static_assert(anne::kRpmbRequestSize == {size});
static_assert(sizeof(rpmb_frame) == 512);
static_assert(sizeof(request_info) == 32);
static_assert(offsetof(rpmb_request, hmac_buf) == 0x4200);
static_assert(offsetof(rpmb_request, info) == 0x6a00);
static_assert(offsetof(rpmb_request, info) == anne::rpmb::kRequestInfoOffset);
static_assert(offsetof(request_info, state) == 20);
static_assert(offsetof(rpmb_request, key_frame_status) == 0x6a20);
static_assert(offsetof(rpmb_request, rpmb_request_status) == 0x6a22);
static_assert(offsetof(rpmb_request, rpmb_exception_status) == 0x6a24);
static_assert(offsetof(rpmb_request, key_frame_status) == anne::rpmb::kKeyStatusOffset);
static_assert(offsetof(rpmb_request, rpmb_request_status) == anne::rpmb::kRequestStatusOffset);
static_assert(offsetof(rpmb_request, rpmb_exception_status) == anne::rpmb::kExceptionStatusOffset);
static_assert(offsetof(rpmb_request, rpmb_debug) == 0x6a28);
static_assert(sizeof(rpmb_request) == 0x6a70);
// Existing research breadcrumbs overlap stock request data. Neither these
// breadcrumbs nor the presumed +0x6c38 callback are a stock interface.
static_assert(0x6900 >= offsetof(rpmb_request, hmac_buf));
static_assert(0x6920 <= offsetof(rpmb_request, info));
static_assert(0x6c38 >= sizeof(rpmb_request));
int main() {{ (void)rpmb_err_msg; }}
'''
        with tempfile.TemporaryDirectory(prefix='ame-rpmb-layout-') as directory:
            cpp, probe = Path(directory) / 'layout.cc', Path(directory) / 'layout'
            cpp.write_text(harness)
            result = subprocess.run([
                'clang++', '-std=c++23', '-Wall', '-Wextra', '-Werror',
                '-I' + str(ROOT / 'kernel/include'), '-I' + str(ROOT / 'boards/huawei-anne'),
                str(cpp), '-o', str(probe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
