from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class Capabilities(unittest.TestCase):
    def test_retention_revocation_and_exhaustion(self):
        with tempfile.TemporaryDirectory(prefix='ame-capabilities-') as work:
            probe = str(Path(work) / 'probe')
            subprocess.run(['clang++', '-std=c++23', '-DANKO_HOST_TEST', '-O1', '-g', '-pthread',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-I'+str(ROOT/'kernel/include'), '-I'+str(ROOT/'shared/include'),
                str(ROOT/'tests/capability_probe.cc'),
                str(ROOT/'kernel/cap/cap_table.cc'), str(ROOT/'kernel/cap/object.cc'),
                '-o', probe], check=True, capture_output=True)
            result = subprocess.run([probe], capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)
