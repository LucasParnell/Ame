from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ShellDiagnostics(unittest.TestCase):
    def test_reports_and_bounded_log_replay(self):
        with tempfile.TemporaryDirectory(prefix='ame-diag-') as work:
            probe = str(Path(work) / 'probe')
            command = ['clang++', '-std=c++23', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                       '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
            command += ['-I' + str(ROOT / p) for p in
                        ('tests/mochi_host', 'userland/lib/include', 'shared/include')]
            # Keep the host C++ standard headers ahead of freestanding shims.
            command += ['-idirafter', str(ROOT / 'lib/include')]
            command += [str(ROOT / 'tests/msh_diag_probe.cc'),
                        str(ROOT / 'lib/libk/printf.cc'), '-o', probe]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([probe], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
