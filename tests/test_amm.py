from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ArenaMigration(unittest.TestCase):
    def test_mapping_cow_migration_failure_and_retirement(self):
        with tempfile.TemporaryDirectory(prefix='ame-amm-') as directory:
            binary = str(Path(directory) / 'probe')
            result = subprocess.run([
                'clang++', '-std=c++23', '-O1', '-g', '-pthread', '-DANKO_HOST_TEST',
                '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                '-fno-omit-frame-pointer', '-I'+str(ROOT/'kernel/include'),
                '-I'+str(ROOT/'shared/include'), '-idirafter', str(ROOT/'lib/include'),
                str(ROOT/'tests/amm_probe.cc'), str(ROOT/'kernel/mm/amm.cc'),
                str(ROOT/'kernel/mm/adma.cc'), str(ROOT/'kernel/cap/object.cc'),
                str(ROOT/'kernel/mm/arena.cc'),
                str(ROOT/'lib/libk/string.cc'), '-o', binary], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([binary], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('amm probe ok', result.stdout)
