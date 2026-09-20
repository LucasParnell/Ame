from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Smmuv3Encoders(unittest.TestCase):
    def test_smmuv3_encoder_probe(self):
        with tempfile.TemporaryDirectory(prefix='ame-smmuv3-') as work:
            probe = str(Path(work) / 'probe')
            subprocess.run(['clang++', '-std=c++23', '-DANKO_HOST_TEST', '-O1', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-Wall', '-Wextra', '-Werror',
                '-I'+str(ROOT/'kernel/include'), '-I'+str(ROOT/'shared/include'),
                '-idirafter', str(ROOT/'lib/include'),
                str(ROOT/'tests/smmuv3_probe.cc'),
                '-o', probe], check=True, capture_output=True, text=True)
            result = subprocess.run([probe], capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('smmuv3 encoder probe ok', result.stdout)
