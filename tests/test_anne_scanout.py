from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class AnneScanout(unittest.TestCase):
    def test_register_plan_and_retirement_state_machine(self):
        with tempfile.TemporaryDirectory() as work:
            probe = str(Path(work) / 'probe')
            subprocess.run(['clang++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra',
                            '-Werror', '-fsanitize=address,undefined',
                            '-I' + str(ROOT / 'boards/huawei-anne'),
                            '-I' + str(ROOT / 'kernel/include'),
                            '-I' + str(ROOT / 'shared/include'),
                            str(ROOT / 'tests/anne_scanout_probe.cc'), '-o', probe], check=True)
            subprocess.run([probe], check=True, timeout=5)
