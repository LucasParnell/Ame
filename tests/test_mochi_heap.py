from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
class MochiHeap(unittest.TestCase):
    def test_heap(self):
        with tempfile.TemporaryDirectory(prefix='ame-mochi-heap-') as directory:
            binary = str(Path(directory) / 'probe')
            result = subprocess.run(['clang++', '-std=c++23', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-I'+str(ROOT/'userland/lib/include'), '-I'+str(ROOT/'shared/include'),
                '-idirafter', str(ROOT/'lib/include'), str(ROOT/'tests/mochi_heap_probe.cc'), '-o', binary],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([binary], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            for failure in ('stale', 'links'):
                result = subprocess.run([binary, failure], capture_output=True, text=True, timeout=15)
                self.assertNotEqual(result.returncode, 0)
                self.assertNotEqual(result.returncode, 2, result.stderr)
                self.assertNotIn('AddressSanitizer:', result.stderr)
