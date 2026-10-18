from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class RuntimeSnapshot(unittest.TestCase):
    def test_bounded_rows_and_generation_reuse(self):
        with tempfile.TemporaryDirectory(prefix='ame-runtime-') as work:
            probe = str(Path(work) / 'probe')
            subprocess.run([
                'clang++', '-std=c++23', '-DANKO_HOST_TEST', '-O1', '-g',
                '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                '-fno-omit-frame-pointer', '-I' + str(ROOT / 'kernel/include'),
                '-I' + str(ROOT / 'shared/include'), '-idirafter', str(ROOT / 'lib/include'),
                str(ROOT / 'tests/runtime_snapshot_probe.cc'), '-o', probe], check=True)
            subprocess.run([probe], check=True, timeout=10)
