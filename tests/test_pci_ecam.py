from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class PciEcam(unittest.TestCase):
    def test_config_isolation_and_abort_decoding(self):
        with tempfile.TemporaryDirectory(prefix='ame-ecam-') as work:
            probe = str(Path(work) / 'probe')
            result = subprocess.run(['clang++', '-std=c++23', '-O1', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-I'+str(ROOT/'kernel/include'), '-I'+str(ROOT/'shared/include'),
                '-idirafter', str(ROOT/'lib/include'),
                str(ROOT/'tests/pci_ecam_probe.cc'), str(ROOT/'kernel/pci/ecam.cc'),
                '-o', probe], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([probe], capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)
