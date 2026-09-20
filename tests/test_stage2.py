from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class Stage2(unittest.TestCase):
    def test_failed_allocations_and_sparse_unmap_reclaim_tables(self):
        with tempfile.TemporaryDirectory(prefix='ame-stage2-') as work:
            probe = str(Path(work) / 'probe')
            result = subprocess.run(['clang++', '-std=c++23', '-O1', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-I'+str(ROOT/'kernel/include'), '-I'+str(ROOT/'shared/include'),
                '-idirafter', str(ROOT/'lib/include'),
                str(ROOT/'tests/stage2_probe.cc'), str(ROOT/'kernel/vm/stage2.cc'),
                '-o', probe], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([probe], capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)
