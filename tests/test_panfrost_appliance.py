"""The render-only appliance must prove a channel without claiming GPU access."""
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('panfrost_acceptance', ROOT / 'scripts/test/dango-panfrost.py')
acceptance = importlib.util.module_from_spec(spec)
spec.loader.exec_module(acceptance)


class PanfrostAppliance(unittest.TestCase):
    def test_phone_renderer_preserves_manager_bootstrap_bindings(self):
        system = json.loads((ROOT / 'userland/system.json').read_text())
        base = next(s for s in system['services'] if s['name'] == 'dango.manager')
        renderer = system['profiles']['huawei-anne-gpu']['overrides']['dango.manager']
        bindings = {b['name']: b for b in renderer['bindings']}
        for binding in base['bindings']:
            self.assertEqual(bindings.get(binding['name']), binding)
        self.assertIn('render.pixels', bindings)
        self.assertIn('diag.rx', bindings)
        self.assertIn('display.tx', bindings)

    def test_real_mali_leaf_types_permissions_and_cleanup(self):
        with tempfile.TemporaryDirectory() as work:
            probe = str(Path(work) / 'probe')
            subprocess.run(['clang++', '-std=c++23', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-DANKO_HOST_TEST', '-I' + str(ROOT / 'kernel/include'),
                '-I' + str(ROOT / 'boards/huawei-anne'), '-I' + str(ROOT / 'shared/include'),
                '-idirafter', str(ROOT / 'lib/include'), str(ROOT / 'tests/mali_shadow_probe.cc'),
                str(ROOT / 'kernel/vm/mali_shadow.cc'), '-o', probe], check=True)
            run = subprocess.run([probe], capture_output=True, text=True, check=True)
            self.assertIn('Mali shadow MMU probe passed', run.stdout)

    def test_channel_requires_both_endpoints_and_no_failure(self):
        logs = ('dango-agent: started\nchannel native probe passed\n'
                'channel guest probe passed: 512 ordered round trips\n'
                'dango: channel test passed\ninit: all services ready\n')
        self.assertTrue(all(acceptance.evidence(logs).values()))
        for marker in ('dango: channel test passed', '512 ordered round trips'):
            self.assertFalse(all(acceptance.evidence(logs.replace(marker, '')).values()))
        self.assertFalse(all(acceptance.evidence(logs + 'USERFAULT').values()))

    def test_minimal_policy_rejects_modules_and_other_gpus(self):
        spec = importlib.util.spec_from_file_location('minimal_policy', ROOT / 'scripts/linux/check-minimal-config.py')
        policy = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(policy)
        config = ''.join(f'CONFIG_{key}=y\n' for key in policy.FLOWS['panfrost']['require'])
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / '.config'
            for addition, valid in (('', True), ('CONFIG_MODULES=y\n', False),
                                    ('CONFIG_DRM_VIRTIO_GPU=y\n', False),
                                    ('CONFIG_PCI=y\n', False)):
                path.write_text(config + addition)
                run = subprocess.run([sys.executable, str(ROOT / 'scripts/linux/check-minimal-config.py'),
                    '--config', str(path), '--flow', 'panfrost'], capture_output=True, text=True)
                self.assertEqual(run.returncode == 0, valid, run.stdout)
            path.write_text(config.replace('CONFIG_DRM_PANFROST=y', 'CONFIG_DRM_PANFROST=m'))
            run = subprocess.run([sys.executable, str(ROOT / 'scripts/linux/check-minimal-config.py'),
                '--config', str(path), '--flow', 'panfrost'], capture_output=True, text=True)
            self.assertNotEqual(run.returncode, 0)

    def test_compressed_root_requires_explicit_policy_and_keeps_device_limits(self):
        spec = importlib.util.spec_from_file_location('minimal_policy', ROOT / 'scripts/linux/check-minimal-config.py')
        policy = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(policy)
        config = ''.join(f'CONFIG_{key}=y\n' for key in policy.FLOWS['panfrost-squashfs']['require'])
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / '.config'
            for flow, addition, valid in (
                ('panfrost', '', False),
                ('panfrost-squashfs', '', True),
                ('panfrost-squashfs', 'CONFIG_USB=y\n', False),
                ('panfrost-squashfs', 'CONFIG_BLK_DEV_RAM=y\n', False),
                ('panfrost-squashfs', 'CONFIG_DRM_PANFROST=m\n', False),
                ('panfrost-squashfs', 'CONFIG_SQUASHFS_XZ=n\n', False),
            ):
                path.write_text(config + addition)
                run = subprocess.run([sys.executable, str(ROOT / 'scripts/linux/check-minimal-config.py'),
                    '--config', str(path), '--flow', flow], capture_output=True, text=True)
                self.assertEqual(run.returncode == 0, valid, run.stdout)


class HarnessAgreement(unittest.TestCase):
    """The guest binding and the host mediator must describe one device."""

    def _integer(self, header, name):
        """Read either a #define or a C++ constexpr integer by name."""
        text = (ROOT / header).read_text()
        found = (re.search(rf'^#define\s+{name}\s+(\S+)', text, re.MULTILINE) or
                 re.search(rf'\b{name}\s*=\s*(0[xX][0-9a-fA-F]+|\d+)', text))
        self.assertIsNotNone(found, f'{name} missing from {header}')
        return int(found.group(1).rstrip('uUlL'), 0)

    def test_mediator_constants_match_the_vendor_interrupt_list(self):
        # anne::gpu derives host INTIDs from the vendor SPIs; the appliance ABI
        # publishes the guest-visible vGIC numbers. Both must stay consistent
        # with the vendor DTS, which is the only place they come from.
        gpu_h = (ROOT / 'boards/huawei-anne/gpu.h').read_text()
        spi = int(re.search(r'kGicSpiBase = (\d+)', gpu_h).group(1))
        for name, vendor_spi in (('Job', 258), ('Mmu', 259), ('Gpu', 260)):
            m = re.search(rf'kHostIrq{name} = kGicSpiBase \+ (\d+)', gpu_h)
            self.assertEqual(int(m.group(1)), vendor_spi)
            self.assertEqual(spi + vendor_spi, 32 + vendor_spi)
        # The guest window must equal the vendor register range.
        self.assertEqual(self._integer('boards/huawei-anne/gpu.h', 'kBytes'), 0x4000)
        self.assertEqual(self._integer('shared/include/midix/abi/anne_gpu.h',
                                       'AME_MALI_REG_BYTES'), 0x4000)

    @unittest.skipUnless(shutil.which('dtc') and shutil.which('fdtget'),
                         'requires device-tree tools')
    def test_guest_gpu_node_is_derived_from_the_abi_header(self):
        header = ROOT / 'shared/include/midix/abi/anne_gpu.h'
        header_angle = 'shared/include/midix/abi/anne_gpu.h'
        base = self._integer(header_angle, 'AME_MALI_GUEST_BASE')
        bytes_ = self._integer(header_angle, 'AME_MALI_REG_BYTES')
        irqs = [self._integer(header_angle, n)
                for n in ('AME_MALI_JOB_IRQ', 'AME_MALI_MMU_IRQ', 'AME_MALI_GPU_IRQ')]
        with tempfile.TemporaryDirectory(prefix='ame-mali-dtb-') as directory:
            root = Path(directory)
            (root / 'initramfs.cpio').write_bytes(b'initramfs-size-fixture')
            env = dict(os.environ, DANGO_LINUX_OUT=directory, DANGO_LINUX_GUEST='minimal',
                       DANGO_DTS_TEMPLATE=str(ROOT / 'linux-artifacts/dts/dango-linux-panfrost.dts.in'))
            dtb = root / 'dango-linux.dtb'
            node = f'/gpu@{base:x}'

            # Default: the node exists but claims nothing, because QEMU has no
            # Mali and an unassigned GPU must never read as ready.
            run = subprocess.run([str(ROOT / 'scripts/linux/build-dtb.sh')], env=env,
                                 capture_output=True, text=True, timeout=60)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertEqual(subprocess.run(['fdtget', str(dtb), node, 'status'],
                capture_output=True, text=True).stdout.strip(), 'disabled')

            run = subprocess.run([str(ROOT / 'scripts/linux/build-dtb.sh')],
                                 env=dict(env, DANGO_PANFROST_HARDWARE='1'),
                                 capture_output=True, text=True, timeout=60)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertEqual(subprocess.run(['fdtget', str(dtb), node, 'status'],
                capture_output=True, text=True).stdout.strip(), 'okay')
            reg = subprocess.run(['fdtget', '-t', 'x', str(dtb), node, 'reg'],
                                 capture_output=True, text=True)
            self.assertEqual(reg.stdout.split(), ['0', f'{base:x}', '0', f'{bytes_:x}'])
            ints = subprocess.run(['fdtget', '-t', 'x', str(dtb), node, 'interrupts'],
                                  capture_output=True, text=True).stdout.split()
            # Three <0 spi flags> triples: interrupt type 0 (SPI), then the DTS
            # SPI number, which is the INTID minus the GIC SPI base, then the
            # vendor's level-high flags cell.
            expected = []
            for irq in irqs:
                expected.extend(['0', f'{irq - 32:x}', '4'])
            self.assertEqual(ints, expected)
            supply = subprocess.run(['fdtget', '-t', 'x', str(dtb), node, 'mali-supply'],
                                    capture_output=True, text=True, check=True).stdout.strip()
            phandle = subprocess.run(['fdtget', '-t', 'x', str(dtb), '/gpu-supply', 'phandle'],
                                     capture_output=True, text=True, check=True).stdout.strip()
            self.assertEqual(supply, phandle)
            self.assertEqual(subprocess.run(['fdtget', str(dtb), '/gpu-supply', 'compatible'],
                capture_output=True, text=True, check=True).stdout.strip(), 'regulator-fixed')
            for property_ in ('regulator-always-on', 'regulator-boot-on'):
                subprocess.run(['fdtget', str(dtb), '/gpu-supply', property_], check=True,
                               capture_output=True, text=True)

    def test_retention_gate_precedes_stage2_teardown(self):
        # A GPU owner must be released before any backing it can DMA from is
        # destroyed, and before the VM state changes, so a refused reset leaves
        # the VM inspectable and retryable.
        text = (ROOT / 'kernel/vm/vm.cc').read_text()
        body = text[text.index('static void retire_vm'):]
        body = body[:body.index('\n}\n')]
        self.assertLess(body.index('mali::release(*v)'), body.index('v->state = VM_DEAD'))
        self.assertLess(body.index('mali::release(*v)'), body.index('stage2_destroy'))
        self.assertLess(body.index('mali::release(*v)'), body.index('device_release_for_vm'))

    def test_serial_console_is_not_keyboard_gated(self):
        # A USB-console board has no keyboard, so the console must open on an
        # attached serial host. Regression for a deadlock: the FLOW_ON control
        # record that authorises the byte stream was dropped by the very gate it
        # was meant to open, and only three ESC keycodes could clear it.
        text = (ROOT / 'userland/drivers/tty/main.cc').read_text()
        live = text[text.index('static inline bool console_live()'):]
        live = live[:live.index('\n}')]
        self.assertIn('serial_attached', live,
                      'console_live() must also open for an attached serial host')
        # Control must be consumed before the gate is consulted, otherwise the
        # generation that opens it can never arrive.
        body = text[text.index('MxRingSlot slot{};'):]
        body = body[:body.index('return true; // a consumed non-text/control record')]
        self.assertLess(body.index('MX_TERM_CONTROL'), body.index('console_live()'),
                        'session control must be accepted before the gate')

    def test_syscall_number_is_unique(self):
        # A duplicate case value is a compile error, but the header is the
        # source both sides agree on, so guard it directly.
        text = (ROOT / 'shared/include/midix/abi/syscall.h').read_text()
        pairs = re.findall(r'MX_NR_([A-Z_0-9]+)\s*=\s*(\d+)', text)
        seen = {}
        for name, value in pairs:
            self.assertNotIn(value, seen, f'MX_NR_{name} duplicates MX_NR_{seen.get(value)}')
            seen[value] = name


if __name__ == '__main__':
    unittest.main()
