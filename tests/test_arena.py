from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Arena(unittest.TestCase):
    def test_arena_is_bounded_and_refuses_growth(self):
        # mochi/arena.h is the libc-allocation replacement: bounded, aligned,
        # never grows, refuses an unsatisfiable request. Prove it under
        # ASan/UBSan.
        with tempfile.TemporaryDirectory(prefix='ame-arena-') as work:
            probe = str(Path(work) / 'probe')
            result = subprocess.run([
                'clang++', '-std=c++23', '-O1', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-I' + str(ROOT / 'lib/include'),
                '-I' + str(ROOT / 'shared/include'),
                '-I' + str(ROOT / 'userland/lib/include'),
                str(ROOT / 'tests/arena_probe.cc'), '-o', probe],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([probe], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
