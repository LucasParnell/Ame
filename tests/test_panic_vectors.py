from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

# The AArch64 vector table is 4 groups of 4 at 128-byte spacing; the kernel must
# name every slot so an unexpected exception reports something meaningful.
EXPECTED = {
    7: 'SError',
    11: 'SError',
    6: 'FIQ',
    3: 'SError',
}


class ExceptionVectors(unittest.TestCase):
    def test_every_vector_slot_has_a_reason(self):
        compiler = shutil.which('clang++') or shutil.which('g++')
        if not compiler:
            raise unittest.SkipTest('no host C++ compiler')
        with tempfile.TemporaryDirectory(prefix='ame-vectors-') as tmp:
            probe = Path(tmp) / 'vectors_probe'
            subprocess.run([
                compiler, '-std=c++23', '-O1',
                '-I' + str(ROOT / 'kernel/include'),
                str(ROOT / 'tests/vectors_probe.cc'), '-o', str(probe)],
                check=True, capture_output=True)
            result = subprocess.run([str(probe)], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)

        reasons = {}
        for line in result.stdout.strip().splitlines():
            index, _, reason = line.partition('|')
            reasons[int(index)] = reason

        self.assertEqual(len(reasons), 16, 'all 16 vector slots must be named')
        for index, reason in reasons.items():
            self.assertTrue(reason.strip(), f'slot {index} has an empty reason')
        for index, needle in EXPECTED.items():
            self.assertIn(needle, reasons[index], f'slot {index} should describe a {needle}')
        # The slots the kernel handles itself must not be claimed as "unexpected".
        for index in (4, 5, 8):
            self.assertNotIn('unexpected', reasons[index].lower(),
                             f'slot {index} is handled by a dedicated entry')


if __name__ == '__main__':
    unittest.main()
