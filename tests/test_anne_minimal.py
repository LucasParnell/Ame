"""Exercise the minimal native entry and framebuffer without Huawei peripherals."""
import json
from pathlib import Path
import shutil
import select
import struct
import re
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
LLVM = Path('/opt/homebrew/opt/llvm/bin')
LLD = shutil.which('ld.lld') or '/opt/homebrew/opt/lld/bin/ld.lld'


class MinimalFramebuffer(unittest.TestCase):
    def test_source_bounds_formats_and_rendering(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / 'framebuffer')
            subprocess.run(['clang++', '-std=c++23', '-O1', '-fsanitize=address,undefined',
                            '-I' + str(ROOT / 'boards/huawei-anne/minimal'),
                            str(ROOT / 'tests/anne_minimal_framebuffer_probe.cc'), '-o', binary],
                           check=True, capture_output=True)
            subprocess.run([binary], check=True, capture_output=True, timeout=10)


@unittest.skipUnless(shutil.which('qemu-system-aarch64') and (LLVM / 'clang++').exists() and Path(LLD).exists(),
                     'requires QEMU and the AArch64 LLVM toolchain')
class MinimalEntry(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory(prefix='ame-minimal-')
        cls.path = Path(cls.work.name)
        sources = ['boards/huawei-anne/minimal/entry.S', 'boards/huawei-anne/minimal/main.cc',
                   'boards/huawei-anne/native_log.cc', 'kernel/init/fdt.cc', 'lib/libk/string.cc']
        flags = ['--target=aarch64-none-elf', '-std=c++23', '-O2', '-ffreestanding', '-nostdinc++',
                 '-mgeneral-regs-only', '-mstrict-align', '-fno-exceptions', '-fno-rtti',
                 '-fno-stack-protector', '-fno-pie', '-DANNE_MINIMAL_TEST=1',
                 '-I' + str(ROOT / 'kernel/include'), '-I' + str(ROOT / 'lib/include'),
                 '-I' + str(ROOT / 'boards/huawei-anne')]
        for fault in [False, True]:
            objects = []
            for i, source in enumerate(sources):
                obj = cls.path / f'{fault}-{i}.o'
                subprocess.run([str(LLVM / 'clang++'), *flags,
                                *(['-DANNE_MINIMAL_TEST_FAULT=1'] if fault else []),
                                '-c', str(ROOT / source), '-o', str(obj)], check=True, capture_output=True)
                objects.append(str(obj))
            elf = cls.path / f'entry-{fault}.elf'
            subprocess.run([LLD, '-static', '--defsym=ANNE_IMAGE_BASE=0x40080000',
                            '-T', str(ROOT / 'boards/huawei-anne/minimal/minimal.ld'),
                            *objects, '-o', str(elf)], check=True, capture_output=True)
            subprocess.run([str(LLVM / 'llvm-objcopy'), '-O', 'binary', str(elf),
                            str(cls.path / f'entry-{fault}.bin')], check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def run_entry(self, el, fault=False, channel=0x53000):
        with tempfile.TemporaryDirectory(dir=self.path) as directory:
            path = Path(directory)
            rdma = bytearray(0xd8)
            for offset, value in {0: 0, 4: 0, 8: 269, 12: 2279, 0x1c: 5 << 3,
                                  0x60: 0x41000000, 0x64: 270, 0xd4: 1}.items():
                struct.pack_into('<I', rdma, offset, value)
            ldi = bytearray(0x28)
            for offset, value in {8: 1079, 0x14: 2279, 0x24: 1}.items():
                struct.pack_into('<I', ldi, offset, value)
            (path / 'rdma').write_bytes(rdma); (path / 'ldi').write_bytes(ldi)
            command = ['qemu-system-aarch64', '-machine', f'virt,gic-version=2,virtualization={"on" if el == 2 else "off"}',
                       '-cpu', 'cortex-a53', '-m', '128M', '-smp', '1', '-display', 'none',
                       '-serial', 'none', '-monitor', 'none', '-S', '-qmp', 'stdio',
                       '-kernel', str(self.path / f'entry-{fault}.bin'),
                       '-device', f'loader,file={path / "rdma"},addr={0x42000000 + channel:#x}',
                       '-device', f'loader,file={path / "ldi"},addr=0x4207d000']
            proc = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, bufsize=0)
            try:
                deadline = time.monotonic() + 12
                def receive():
                    self.assertTrue(select.select([proc.stdout], [], [], 3)[0], 'QMP response timeout')
                    line = proc.stdout.readline()
                    if not line: self.fail(proc.stderr.read().decode())
                    return json.loads(line)
                receive()
                def qmp(name, arguments=None):
                    proc.stdin.write((json.dumps({'execute': name, 'arguments': arguments or {}}) + '\n').encode())
                    while True:
                        response = receive()
                        if 'event' in response: continue
                        self.assertNotIn('error', response); return response.get('return')
                qmp('qmp_capabilities'); qmp('cont')
                expected = 0xff00ff00 if el == 1 else 0xffff00ff
                sample = path / 'sample'
                while time.monotonic() < deadline:
                    # Wait for every LED transition (including the intentional
                    # fault path) and reset request to finish.
                    self.assertEqual(qmp('human-monitor-command', {'command-line':
                        f'pmemsave 0x44060000 4096 "{sample}"'}), '')
                    record = sample.read_bytes()[12:].decode('ascii', errors='replace')
                    if 'nmi=0x' in record: break
                    time.sleep(0.05)
                else: self.fail(f'entry at EL{el} did not complete its diagnostic stages')
                self.assertIn('AMEFL6', record)
                held = {stage: int(ticks) for stage, ticks in re.findall(r'hold=([rbgw]) end=0x[0-9a-f]+ ticks=([0-9]+)', record)}
                for stage in ('r', 'b', 'w' if fault else 'g'):
                    self.assertIn(stage, held, f'missing LED transition {stage}')
                    # Normal transitions take milliseconds; a one-second upper
                    # bound allows emulator jitter but catches the old 3 s dwell.
                    self.assertLess(held[stage], 1920000)
                if fault: self.assertIn('!FAULT', record)
                else: self.assertNotIn('!FAULT', record)
                self.assertEqual(qmp('human-monitor-command', {'command-line':
                    f'pmemsave {0x41000000 + 2279 * 4320:#x} 4 "{sample}"'}), '')
                self.assertEqual(struct.unpack('<I', sample.read_bytes())[0], expected)
                # Verify glyph A: black margin, white top stroke, black interior.
                row = path / 'row'
                self.assertEqual(qmp('human-monitor-command', {'command-line':
                    f'pmemsave {0x41000000 + 8 * 4320:#x} 128 "{row}"'}), '')
                pixels = struct.unpack('<32I', row.read_bytes())
                self.assertEqual(pixels[0], 0xff000000)
                self.assertEqual(pixels[8], 0xffffffff)
                qmp('quit')
            finally:
                if proc.poll() is None: proc.terminate()
                proc.communicate(timeout=5)

    def test_el1_entry_and_framebuffer(self): self.run_entry(1)
    def test_el2_entry_and_framebuffer(self): self.run_entry(2)
    def test_alternate_active_channel(self): self.run_entry(1, channel=0x52000)
    def test_el1_exception_vectors(self): self.run_entry(1, fault=True)
    def test_el2_exception_vectors(self): self.run_entry(2, fault=True)


if __name__ == '__main__': unittest.main()
