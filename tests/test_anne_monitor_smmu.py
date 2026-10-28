from pathlib import Path
import importlib.util
import hashlib
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SecureSmmuProbe(unittest.TestCase):
    def test_firmware_patch_scope_and_input_rejection(self):
        spec = importlib.util.spec_from_file_location('monitor_builder', ROOT / 'scripts/anne/build-monitor-probe.py')
        builder = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(builder)
        with tempfile.TemporaryDirectory() as work:
            work = Path(work)
            monitor = bytearray(0x1bb00)
            struct.pack_into('<QQQQ', monitor, 0x13570, 0xfffc0000, 0xfffc0000, 0xd000, 8)
            struct.pack_into('<QQQQ', monitor, 0x13630, 0xfffc0000, 0xfffc0000, 0x8000, 8)
            struct.pack_into('<I', monitor, 0x1a800,
                0x94000000 | (((0x11574 - 0x1a800) // 4) & 0x3ffffff))
            loader = bytearray(3624960)
            struct.pack_into('<I', loader, builder.PATCH, 0x17fbd30a)
            struct.pack_into('<Q', loader, 0x3e0, 0x2bbcb0)
            original, lk = work / 'original', work / 'lk'
            original.write_bytes(monitor)
            lk.write_bytes(loader)
            with self.assertRaisesRegex(ValueError, 'Unreviewed input'):
                builder.build(original, lk, work / 'rejected', 'smmu-id')
            # Synthetic fixture is explicitly pinned only inside this test.
            builder.BL31_SHA = hashlib.sha256(monitor).hexdigest()
            builder.LK_SHA = hashlib.sha256(loader).hexdigest()
            report = builder.build(original, lk, work / 'profile', 'smmu-id')
            updated = (work / 'profile/monitor.bin').read_bytes()
            allowed = set(range(builder.CALLBACK, builder.CALLBACK + report['callback_bytes']))
            allowed.update(range(0x13630, 0x13650))
            allowed.update(range(0x1a800, 0x1a800 + report['vector_bytes']))
            changed = {i for i, (a, b) in enumerate(zip(monitor, updated)) if a != b}
            self.assertTrue(changed and changed <= allowed)
            self.assertEqual(len(updated), len(monitor))
            self.assertEqual(updated[0x13570:0x13590], monitor[0x13570:0x13590])
            self.assertEqual(report['signature'], 0x414d0004)
            self.assertTrue(report['read_only'])
            self.assertNotIn('statistics_writable_mask', report)
            self.assertEqual(struct.unpack_from('<QQQQ', updated, 0x13630),
                             (0xe8dc0000, 0xe8dc0000, 0x1000, 0))
            with self.assertRaisesRegex(ValueError, 'Unknown bounded'):
                builder.build(original, lk, work / 'bad', 'arbitrary')

    @unittest.skipUnless(shutil.which('qemu-system-aarch64'), 'requires QEMU system emulator')
    def test_real_el3_recovery_and_unrelated_fault(self):
        spec = importlib.util.spec_from_file_location('monitor_builder', ROOT / 'scripts/anne/build-monitor-probe.py')
        builder = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(builder)
        with tempfile.TemporaryDirectory() as work:
            work = Path(work)
            probe = work / 'probe.bin'
            builder.assemble(ROOT / 'scripts/anne/monitor_smmu_probe.S', probe, 0x2000)
            names = {}
            for line in subprocess.check_output(['llvm-nm', str(probe.with_suffix('.elf'))], text=True).splitlines():
                fields = line.split()
                if len(fields) == 3:
                    names[fields[2]] = int(fields[0], 16)
            guard = builder.assemble(ROOT / 'scripts/anne/monitor_smmu_recover.S', work / 'guard.bin', 0x800,
                f'probe_load = {names["smmu_probe_load"]}; probe_return = {names["smmu_probe_return"]}; stock_unhandled = 0x1800;')
            self.assertLessEqual(len(guard), 128)
            for unrelated in (False, True):
                linker = work / 'test.ld'
                linker.write_text('SECTIONS { . = 0; .start : { *(.text.start) } '
                    '. = 0x800; vectors = .; .vector : { *(.text.vector) } '
                    '. = 0x1800; .fallback : { *(.text.fallback) } '
                    '. = 0x2000; .probe : { *(.text.probe) } }')
                # Keep actual production instructions; only their placement and
                # the harness's fatal target differ from the handset profile.
                assembly = work / 'all.S'
                assembly.write_text((ROOT / 'tests/monitor_smmu_el3.S').read_text().replace(
                    '.global _start, stock_unhandled', '.global _start, stock_unhandled_real').replace(
                    'stock_unhandled:', 'stock_unhandled_real:') +
                    '\n.section .text.vector, "ax"\n.incbin "' + str(work / 'guard.bin') + '"\n' +
                    '.section .text.fallback, "ax"\nb stock_unhandled_real\n' +
                    '.section .text.probe, "ax"\n.global monitor_probe\nmonitor_probe:\n.incbin "' + str(probe) + '"\n')
                elf = work / 'test.elf'
                subprocess.run(['clang', '--target=aarch64-none-elf', '-nostdlib', '-fuse-ld=lld',
                    '-Wl,--build-id=none', '-Wl,-T,' + str(linker),
                    *(['-DTEST_UNRELATED'] if unrelated else []), str(assembly), '-o', str(elf)], check=True)
                image = work / 'test.bin'
                subprocess.run(['llvm-objcopy', '-O', 'binary', str(elf), str(image)], check=True)
                result = subprocess.run(['qemu-system-aarch64', '-machine', 'virt,secure=on,virtualization=on',
                    '-cpu', 'cortex-a53', '-m', '128M', '-nographic', '-monitor', 'none', '-serial', 'none',
                    '-semihosting-config', 'enable=on,target=native', '-bios', str(image)],
                    capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                expected = 'unrelated EL3 fault kept fatal' if unrelated else 'EL3 fault recovery passed'
                self.assertIn(expected, result.stdout + result.stderr)
