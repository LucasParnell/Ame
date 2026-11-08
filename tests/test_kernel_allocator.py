from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class KernelManagedAllocation(unittest.TestCase):
    def test_type_isolation_reuse_and_corruption_rejection(self):
        with tempfile.TemporaryDirectory(prefix='ame-kernel-arena-') as directory:
            binary = str(Path(directory) / 'probe')
            built = subprocess.run([
                'clang++', '-std=c++23', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-I'+str(ROOT/'kernel/include'), '-I'+str(ROOT/'shared/include'),
                '-idirafter', str(ROOT/'lib/include'), str(ROOT/'tests/kernel_allocator_probe.cc'),
                '-o', binary], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            run = subprocess.run([binary], capture_output=True, text=True, timeout=20)
            self.assertEqual(run.returncode, 0, run.stderr)
            for corruption in ('type', 'double', 'overrun', 'stale'):
                run = subprocess.run([binary, corruption], capture_output=True, text=True, timeout=20)
                self.assertEqual(run.returncode, 77, run.stderr)
