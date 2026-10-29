from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class VmProgress(unittest.TestCase):
    def test_concurrent_snapshot_and_stuck_writer_are_bounded(self):
        with tempfile.TemporaryDirectory(prefix='ame-vm-progress-') as work:
            probe = str(Path(work) / 'probe')
            compiled = subprocess.run([
                'clang++', '-std=c++23', '-O2', '-g', '-pthread', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-I'+str(ROOT/'kernel/include'), '-I'+str(ROOT/'shared/include'),
                '-idirafter', str(ROOT/'lib/include'), str(ROOT/'tests/vm_progress_probe.cc'),
                '-o', probe], capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            result = subprocess.run([probe], capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('vm progress probe ok', result.stdout)
