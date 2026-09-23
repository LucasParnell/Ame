from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Libk(unittest.TestCase):
    def test_safe_bytes_str_num(self):
        # libk/bytes.h, libk/str.h, libk/num.h are the caller-facing safe layer;
        # prove their bounds behaviour under ASan/UBSan (a wrong length must be a
        # short count or a false, never an out-of-bounds access).
        with tempfile.TemporaryDirectory(prefix='ame-libk-') as work:
            probe = str(Path(work) / 'probe')
            result = subprocess.run([
                'clang++', '-std=c++23', '-O1', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-I' + str(ROOT / 'lib/include'),
                str(ROOT / 'tests/libk_probe.cc'), '-o', probe],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([probe], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
