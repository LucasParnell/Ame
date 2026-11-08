from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class HealthAuthority(unittest.TestCase):
    def test_health_observer_requires_read_authority(self):
        with tempfile.TemporaryDirectory(prefix='ame-health-authority-') as directory:
            probe = Path(directory) / 'probe'
            command = [
                'clang++', '-std=c++23', '-DANKO_HOST_TEST', '-O1', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-ffunction-sections', '-fdata-sections', '-Wall', '-Wextra', '-Werror',
                '-I' + str(ROOT / 'kernel/include'), '-I' + str(ROOT / 'shared/include'),
                '-idirafter', str(ROOT / 'lib/include'),
                str(ROOT / 'tests/health_authority_probe.cc'),
                str(ROOT / 'kernel/cap/cap_table.cc'), str(ROOT / 'kernel/cap/object.cc'),
                '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections',
                '-o', str(probe),
            ]
            compiled = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            result = subprocess.run([str(probe)], capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('Health observer authority probe ok', result.stdout)
