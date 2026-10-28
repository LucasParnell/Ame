from pathlib import Path
import importlib.util
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MonitorProbe(unittest.TestCase):
    @unittest.skipUnless(shutil.which('qemu-aarch64') and shutil.which('aarch64-linux-gnu-gcc'),
                         'requires AArch64 userspace emulator and compiler')
    def test_bounded_reads_and_preserved_handoff(self):
        spec = importlib.util.spec_from_file_location('monitor_builder', ROOT / 'scripts/anne/build-monitor-probe.py')
        builder = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(builder)
        with tempfile.TemporaryDirectory() as work:
            work = Path(work)
            probe, preload = work / 'probe.bin', work / 'preload.bin'
            builder.assemble(ROOT / 'scripts/anne/monitor_probe.S', probe, 0x35e058e8)
            builder.assemble(ROOT / 'scripts/anne/monitor_preload.S', preload, 0x2bc100,
                             'monitor_bytes = 113408;')
            self.assertLessEqual(probe.stat().st_size, 0x5a10 - 0x58e8)
            self.assertLessEqual(preload.stat().st_size, 0x100)
            subprocess.run(['aarch64-linux-gnu-gcc', '-O2', '-Wall', '-Wextra', '-Werror',
                            str(ROOT / 'tests/monitor_probe_test.c'), '-o', str(work / 'test')], check=True)
            result = subprocess.run(['qemu-aarch64', '-L', '/usr/aarch64-linux-gnu',
                                     str(work / 'test'), str(probe), str(preload)],
                                    capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('preload handoff passed', result.stdout)
