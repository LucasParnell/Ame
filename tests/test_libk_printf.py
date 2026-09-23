from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class LibkPrintf(unittest.TestCase):
    def test_bounded_formatter_and_checked_math(self):
        # libk's snprintf writes at most cap-1 bytes, NUL-terminates, and returns
        # the would-be length; the checked integer helpers never wrap. Prove it
        # under ASan/UBSan.
        with tempfile.TemporaryDirectory(prefix='ame-printf-') as work:
            probe = str(Path(work) / 'probe')
            result = subprocess.run([
                'clang++', '-std=c++23', '-O1', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-I' + str(ROOT / 'lib/include'),
                str(ROOT / 'tests/printf_probe.cc'),
                str(ROOT / 'lib/libk/printf.cc'), '-o', probe],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([probe], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
