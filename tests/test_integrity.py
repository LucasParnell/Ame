from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Integrity(unittest.TestCase):
    def test_polynomials_streaming_progress_and_overflowing_zero_range(self):
        with tempfile.TemporaryDirectory(prefix='ame-integrity-') as directory:
            binary = Path(directory) / 'probe'
            subprocess.run([
                'clang++', '-std=c++23', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-I' + str(ROOT / 'shared/include'), '-idirafter', str(ROOT / 'lib/include'),
                str(ROOT / 'tests/integrity_probe.cc'), str(ROOT / 'shared/format/integrity.cc'),
                '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)
