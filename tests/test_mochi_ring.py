from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MochiRing(unittest.TestCase):
    def test_messages_rings_and_protocol_replies(self):
        with tempfile.TemporaryDirectory(prefix='ame-mochi-ring-') as work:
            probe = str(Path(work) / 'probe')
            result = subprocess.run([
                'clang++', '-std=c++23', '-O1', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-I' + str(ROOT / 'tests/mochi_host'),
                '-I' + str(ROOT / 'userland/lib/include'),
                '-I' + str(ROOT / 'shared/include'),
                '-I' + str(ROOT / 'lib/include'),
                str(ROOT / 'tests/mochi_ring_probe.cc'), '-o', probe],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([probe], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
