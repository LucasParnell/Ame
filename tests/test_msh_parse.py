from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ShellParsing(unittest.TestCase):
    def test_expansion_and_assignment_bounds(self):
        with tempfile.TemporaryDirectory(prefix='ame-shell-') as work:
            probe = str(Path(work) / 'probe')
            command = ['clang++', '-std=c++23', '-O1', '-g',
                       '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
            command += ['-I' + str(ROOT / p) for p in
                        ('tests/mochi_host', 'userland/lib/include', 'shared/include', 'lib/include')]
            command += [str(ROOT / 'tests/msh_parse_probe.cc'),
                        str(ROOT / 'lib/libk/printf.cc'), '-o', probe]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([probe], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
