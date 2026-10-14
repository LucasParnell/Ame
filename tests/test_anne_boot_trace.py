"""Run full Anko with the native trace transport on synthetic QEMU peripherals."""
import json
from pathlib import Path
import re
import select
import shutil
import struct
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'out/anne-trace-qemu-dev'


@unittest.skipUnless(shutil.which('cmake') and shutil.which('qemu-system-aarch64'), 'requires CMake and QEMU')
class NativeBootTrace(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.configure(0)

    @classmethod
    def configure(cls, fault, capture=0, preset="anne-trace-qemu-dev", display=False, frame_console=False, reset_at_ready=True, watchdog=False, retention=False):
        for command in [['cmake', '--preset', preset,
                         f'-DANKO_ANNE_DIAG_QEMU_FAULT_STAGE={fault}',
                         f'-DANKO_ANNE_DIAG_CAPTURE_SECONDS={capture}',
                         f'-DANKO_ANNE_DIAG_RESET_AT_READY={"ON" if reset_at_ready else "OFF"}',
                         f'-DANKO_ANNE_WATCHDOG={"ON" if watchdog else "OFF"}',
                         f'-DANKO_ANNE_DIAG_WATCHDOG_RETENTION={"ON" if retention else "OFF"}',
                         f'-DANKO_ANNE_DISPLAY_CAPTURE={"ON" if display else "OFF"}',
                         f'-DANKO_ANNE_FRAMEBUFFER={"ON" if frame_console else "OFF"}',
                         '-DANKO_ANNE_DIAG_READY_HOLD_SECONDS=0'],
                        ['cmake', '--build', '--preset', preset, '--target', 'anko']]:
            r = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=120)
            if r.returncode: raise RuntimeError(r.stdout[-3000:] + r.stderr[-3000:])

    @classmethod
    def tearDownClass(cls):
        cls.configure(0)

    def trace(self, el, fault=0, capture=0, cpus=1, workload=False, display=False, iova=False, frame_console=False, reset_at_ready=True, fiq_source=0, watchdog=False, watchdog_control=3, retention=False):
        preset = "anne-smp-trace-qemu-dev" if workload else "anne-trace-qemu-dev"
        build = ROOT / "out/anne-smp-trace-qemu-dev" if workload else BUILD
        # A readiness capture's interval is a boot deadline, not the runtime
        # observation duration. Eight emulated CPUs and display rendering need
        # startup headroom even on a busy host; the same assertions still apply.
        boot_capture = max(capture, 30) if capture and reset_at_ready and cpus > 1 else capture
        self.configure(fault, boot_capture, preset, display, frame_console, reset_at_ready, watchdog, retention)
        retention_wait = retention and bool(watchdog_control)
        end_marker = 'RETENTION watchdog.expiry deliberately_requested=1' if retention_wait else 'RESET nmi=0x4' if fault or capture else 'LED g ctl='
        with tempfile.TemporaryDirectory(prefix='ame-boot-trace-') as directory:
            path = Path(directory) / 'record'
            command = ['qemu-system-aarch64', '-machine',
                       f'virt,gic-version=2,virtualization={"on" if el == 2 else "off"}',
                       '-cpu', 'cortex-a53', '-m', '128M', '-smp', str(cpus), '-display', 'none',
                       '-serial', 'none', '-monitor', 'none', '-S', '-qmp', 'stdio',
                       '-kernel', str(build / 'kernel/anko.bin'),
                       '-device', f'loader,addr=0x47400000,data={fiq_source:#x},data-len=8']
            if watchdog:
                fixture = bytearray(4096)
                registers = {0: 0x10000000, 4: 0x8000000, 8: watchdog_control, 0xfe0: 5, 0xfe4: 0x18, 0xfe8: 0x14}
                for offset, value in registers.items(): struct.pack_into('<I', fixture, offset, value)
                fixture_path = Path(directory) / 'watchdog'; fixture_path.write_bytes(fixture)
                command += ['-device', f'loader,file={fixture_path},addr=0x47500000']
            if display or frame_console:
                channel_index, channel_offset = (6, 0x50000) if frame_console else (1, 0x53000)
                modules = {
                    'rdma': (0x47200000 + channel_offset, {0: 0, 4: 0, 8: 269, 12: 2279, 0x1c: (5 << 3) | (256 if iova else 0),
                                        0x60: 0x43000000, 0x64: 270, 0xd4: 1, 0x100: (1079 << 16) | 2279}),
                    'ldi': (0x4727d000, {8: 1079, 0x14: 2279, 0x24: 1}),
                    'dsi': (0x47201000, {4: 1, 0x3c: 1080, 0x60: 2280, 0xb0: 1}),
                    'mctl': (0x47210800, {0: 1, 0xc: 1, 0x10: 1, 0x18: 1}),
                    'sys': (0x47210000, {0x160 + channel_index * 4: 0x102, 0x180: (channel_index << 4) | 8}),
                    'ovl': (0x47260000, {0: (2279 << 16) | 1079, 0xc: (2279 << 16) | 1079,
                                       0x10: 1, 0x18: (2279 << 16) | 1079, 0x34: 1}),
                }
                for name, (address, registers) in modules.items():
                    fixture = bytearray(max(registers) + 4)
                    for offset, value in registers.items(): struct.pack_into('<I', fixture, offset, value)
                    fixture_path = Path(directory) / name; fixture_path.write_bytes(fixture)
                    command += ['-device', f'loader,file={fixture_path},addr={address:#x}']
            p = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, bufsize=0)
            def receive():
                self.assertTrue(select.select([p.stdout], [], [], 3)[0], 'QMP timeout')
                line = p.stdout.readline()
                if not line: self.fail(p.stderr.read().decode(errors='replace'))
                return json.loads(line)
            def qmp(name, args=None):
                p.stdin.write((json.dumps({'execute': name, 'arguments': args or {}}) + '\n').encode())
                while True:
                    r = receive()
                    if 'event' in r: continue
                    self.assertNotIn('error', r); return r.get('return')
            record = ''
            try:
                receive(); qmp('qmp_capabilities'); qmp('cont')
                deadline = time.monotonic() + (60 if cpus > 1 or frame_console else 15)
                while time.monotonic() < deadline:
                    self.assertEqual(qmp('human-monitor-command', {'command-line':
                        f'pmemsave 0x46060000 65536 "{path}"'}), '')
                    raw = path.read_bytes(); signature, start, size = struct.unpack_from('<3I', raw)
                    if signature == 0x43474244:
                        self.assertEqual(start, size)
                        record = raw[12:12 + min(size, len(raw) - 12)].decode('ascii', errors='replace')
                    if end_marker in record: break
                    time.sleep(0.05)
                else: self.fail('Boot trace did not reach expected state:\n' + record[-6000:])
                self.assertIn(f'AMEANKO1 el={el}', record)
                self.assertRegex(record, r'CPU_FEATURES midr=0x[0-9a-f]+ mpidr=0x[0-9a-f]+ pfr0=0x[0-9a-f]+ el2_field=[0-9]+')
                if not fault or fault > 26:
                    self.assertIn(f'boot_entry={el} monitor={1 if el == 2 else 0}', record)
                self.assertIn(f'FIQ_SOURCE phase=entry word={fiq_source:#018x}', record)
                if watchdog:
                    expected_status = 1 if watchdog_control else 0
                    self.assertRegex(record, rf'WATCHDOG phase=adopt status={expected_status} id=0x141805 control=0x{watchdog_control:x} load=0x10000000 .* feeds={1 if watchdog_control else 0}')
                self.assertIn('BOOT 2 bss.begin', record)
                self.assertIn('BOOT 3 bss.done', record)
                if workload:
                    self.assertIn('DIAG_READ complete records=8 retained_repeatable=1 bounds_checked=1', record)
                if display:
                    self.assertIn('DISPLAY begin phase=entry', record)
                    self.assertIn('DISPLAY begin phase=mmu.done', record)
                    self.assertIn('DISPLAY paint=skip no_verified_scanout' if iova else 'DISPLAY paint.done label=ANKO', record)
                    if iova: self.assertIn('reject=rdma.iova', record)
                    sample = Path(directory) / 'pixel'
                    self.assertEqual(qmp('human-monitor-command', {'command-line':
                        f'pmemsave {0x43000000 + 2279 * 4320:#x} 4 "{sample}"'}), '')
                    expected = 0 if iova else (0xff07111f if frame_console else (0xff00ff00 if el == 1 else 0xffff00ff))
                    self.assertEqual(struct.unpack('<I', sample.read_bytes())[0], expected)
                    if not iova and not frame_console:
                        self.assertEqual(qmp('human-monitor-command', {'command-line':
                            f'pmemsave {0x43000000 + 144 * 4320 + 72 * 4:#x} 4 "{sample}"'}), '')
                        self.assertEqual(struct.unpack('<I', sample.read_bytes())[0], 0xffffffff)
                if frame_console:
                    if iova:
                        self.assertIn('DISPLAY console=skip no_verified_32bit_scanout', record)
                        self.assertNotIn('DISPLAY console.ready', record)
                    else:
                        self.assertIn('DISPLAY console.ready channel=0x0000000000000006', record)
                        self.assertIn('DISPLAY console.replayed bytes=', record)
                        self.assertRegex(record, r'DISPLAY console.services_ready characters=[1-9][0-9]*')
                        qmp('stop')
                        pixels_path = Path(directory) / 'screen'
                        self.assertEqual(qmp('human-monitor-command', {'command-line':
                            f'pmemsave 0x43000000 {1080 * 2280 * 4} "{pixels_path}"'}), '')
                        pixels = pixels_path.read_bytes()
                        font_source = (ROOT / 'shared/include/midix/font/vga8x16.h').read_text()
                        font = {tuple(int(n, 16) for n in re.findall(r'0x([0-9A-Fa-f]{2})', values)): chr(int(code, 16))
                                for code, values in re.findall(r'/\* 0x([0-9A-Fa-f]{2}) \*/ \{([^}]+)\}', font_source)
                                if 32 <= int(code, 16) <= 126}
                        def read_text(x, y, columns, rows, scale, foreground):
                            lines = []
                            for row in range(rows):
                                line = ''
                                for col in range(columns):
                                    bits = []
                                    for gy in range(16):
                                        value = 0
                                        for gx in range(8):
                                            offset = (y + (row * 16 + gy) * scale) * 4320 + (x + (col * 8 + gx) * scale) * 4
                                            if struct.unpack_from('<I', pixels, offset)[0] == foreground: value |= 0x80 >> gx
                                        bits.append(value)
                                    line += font.get(tuple(bits), '?')
                                lines.append(line.rstrip())
                            return '\n'.join(lines)
                        self.assertEqual(read_text(64, 144, 8, 1, 3, 0xff7ee6a8), f'ANKO EL{el}')
                        rendered = read_text(64, 224, 59, 61, 2, 0xffe6f1ff)
                        self.assertIn('init: all services ready', rendered)
                        self.assertIn('io.console: ready', rendered)
                        self.assertNotIn(' reg=', rendered)
                        # Keep one reviewable QEMU surface for this new renderer.
                        artifact = ROOT / 'out/anne-console-validation'
                        artifact.mkdir(exist_ok=True)
                        (artifact / f'el{el}-rendered.txt').write_text(rendered)
                        (artifact / f'el{el}-framebuffer.bin').write_bytes(pixels)
                if fault:
                    self.assertIn(f'!FAULT last={fault}', record)
                    self.assertRegex(record, r'esr=0x[0-9a-f]{16} pc=0x[0-9a-f]{16}')
                else:
                    self.assertNotIn('!FAULT', record)
                    self.assertIn(f'FIQ_SOURCE phase=mmu.done word={fiq_source:#018x}', record)
                    stages = [int(x) for x in re.findall(r'BOOT ([0-9]+) ', record)]
                    for required in [4,5,6,9,11,14,16,17,21,23,27,30,32,33,34,35,36]:
                        self.assertIn(required, stages, record[-3000:])
                    self.assertIn('init: all services ready', record)
                    initial_pstate = re.search(r'EL0 first_svc=\d+ saved_pstate=(0x[0-9a-f]+)', record)
                    self.assertIsNotNone(initial_pstate)
                    self.assertEqual(int(initial_pstate.group(1), 16) & 0xc0, 0x40, 'FIQ masked; IRQ available to userspace')
                    self.assertIn(f'smp ready online={cpus} mask={((1 << cpus) - 1):x}', record)
                    for cpu in range(cpus):
                        cache = re.search(rf'CACHE cpu={cpu} midr=0x[0-9a-f]+ sctlr=(0x[0-9a-f]+)', record)
                        self.assertIsNotNone(cache, 'Each CPU must report the actual MMU/cache state')
                        self.assertEqual(int(cache.group(1), 16) & 0x1005, 0x1005,
                                         'MMU, data cache and instruction cache must be enabled')
                    if cpus > 1:
                        for cpu in range(1, cpus):
                            self.assertIn(f'CPU {cpu} secondary.entry', record)
                            self.assertIn(f'cpu{cpu} local GIC target={1 << cpu:x} timer ready', record)
                    if workload:
                        self.assertIn('SMP_WORK complete mask=ff workers=8', record)
                        for cpu in range(8):
                            self.assertIn(f'SMP_WORK cpu={cpu} observed={cpu} rounds=', record)
                            self.assertRegex(record, rf'RESSHA cpu={cpu} switches=[1-9][0-9]* preempts=[1-9][0-9]* ticks=[1-9][0-9]*')
                            if cpu: self.assertRegex(record, rf'RESSHA cpu={cpu} .* ipis=[1-9][0-9]*')
                    self.assertIn('LED g ctl=0x0000000000000002 out=0x0000000000000000', record)
                    self.assertLess(record.index('BOOT 36 services.ready'), record.index('LED g '))
                    if capture:
                        self.assertIn('CAPTURE reason=services.ready' if reset_at_ready else 'CAPTURE reason=runtime.observed last=36', record)
                        if retention_wait:
                            self.assertNotIn('RESET nmi=0x4', record)
                            self.assertEqual(record.count('RETENTION watchdog.expiry deliberately_requested=1'), 1)
                            time.sleep(1.2)
                            qmp('human-monitor-command', {'command-line': f'pmemsave 0x46060000 65536 "{path}"'})
                            later = path.read_bytes(); size = struct.unpack_from('<I', later, 8)[0]
                            later = later[12:12 + size].decode('ascii', errors='replace')
                            self.assertNotIn('RESET nmi=0x4', later)
                            self.assertNotIn('!FAULT', later)
                            self.assertEqual(later.count('RUNTIME observed seconds='), 1)
                        else: self.assertIn('RESET nmi=0x4', record)
                        if not reset_at_ready:
                            runtime_mask = re.search(r'RUNTIME observed seconds=\d+ daif=(0x[0-9a-f]+)', record)
                            self.assertIsNotNone(runtime_mask)
                            self.assertTrue(int(runtime_mask.group(1), 16) & 0x40)
                            for cpu in range(cpus):
                                ticks = re.search(rf'RUNTIME cpu={cpu} ticks=(\d+) switches=', record)
                                self.assertIsNotNone(ticks)
                                self.assertGreater(int(ticks.group(1)), 10)
                            if watchdog:
                                observed = re.search(r'WATCHDOG phase=observed status=(\d+) id=0x141805 control=0x([0-9a-f]+) load=0x10000000 .* ris=0x0 feeds=(\d+)', record)
                                self.assertIsNotNone(observed)
                                self.assertEqual(int(observed.group(2), 16), watchdog_control)
                                self.assertGreaterEqual(int(observed.group(3)), 2 if watchdog_control else 0)
                                if not watchdog_control: self.assertEqual(int(observed.group(3)), 0)
                # All five live rings retain the same boot trace, after MMU and
                # process address-space switches on the success path.
                for base in [0x460e0000, 0x46000000, 0x46020000, 0x46040000]:
                    self.assertEqual(qmp('human-monitor-command', {'command-line':
                        f'pmemsave {base:#x} 65536 "{path}"'}), '')
                    raw = path.read_bytes(); size = struct.unpack_from('<I', raw, 8)[0]
                    copy = raw[12:12 + min(size, len(raw) - 12)].decode('ascii', errors='replace')
                    self.assertIn('AMEANKO1', copy)
                    self.assertIn(end_marker if fault or capture else 'BOOT 36 services.ready', copy)
                qmp('quit')
            finally:
                if p.poll() is None: p.terminate()
                p.communicate(timeout=5)

    def test_eight_core_el1_workload(self): self.trace(1, cpus=8, workload=True)
    def test_native_display_capture_and_paint(self): self.trace(1, display=True, capture=1, cpus=8, workload=True)
    def test_native_display_capture_and_paint_el2(self): self.trace(2, display=True, capture=1)
    def test_native_display_capture_rejects_iova(self): self.trace(1, display=True, capture=1, iova=True)
    def test_native_framebuffer_console_el1_smp(self): self.trace(1, frame_console=True, display=True, capture=10, cpus=8, workload=True)
    def test_native_framebuffer_console_el2_normal(self): self.trace(2, frame_console=True, capture=10)
    def test_native_framebuffer_console_rejects_iova(self): self.trace(1, frame_console=True, capture=1, iova=True)
    def test_eight_core_el2_workload(self): self.trace(2, cpus=8, workload=True)
    def test_el1_full_boot_logging(self): self.trace(1)
    def test_eight_core_el1_boot(self): self.trace(1, cpus=8)
    def test_eight_core_el2_boot(self): self.trace(2, cpus=8)
    def test_el2_full_boot_logging(self): self.trace(2)
    def test_capture_reset_at_service_readiness(self): self.trace(1, capture=1)
    def test_fault_before_constructors(self): self.trace(1, fault=4)
    def test_fault_after_mmu_enable(self): self.trace(1, fault=17)
    def test_native_fiq_mask_and_runtime_el1(self): self.trace(1, capture=2, cpus=8, frame_console=True, reset_at_ready=False, fiq_source=0x5a5a5a5a5a5a5a4c)
    def test_native_fiq_mask_and_runtime_el2(self): self.trace(2, capture=2, cpus=8, reset_at_ready=False)
    def test_native_watchdog_survives_process_maps_and_smp_runtime(self): self.trace(1, capture=3, cpus=8, frame_console=True, reset_at_ready=False, watchdog=True)
    def test_native_watchdog_does_not_start_inactive_device(self): self.trace(2, capture=2, reset_at_ready=False, watchdog=True, watchdog_control=0)
    def test_watchdog_retention_waits_for_firmware_after_smp_observation(self): self.trace(1, capture=3, cpus=8, frame_console=True, reset_at_ready=False, watchdog=True, retention=True)
    def test_watchdog_retention_falls_back_for_inactive_device(self): self.trace(2, capture=2, reset_at_ready=False, watchdog=True, watchdog_control=0, retention=True)


if __name__ == '__main__': unittest.main()
