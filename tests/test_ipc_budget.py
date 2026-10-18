"""Host test for the userspace poll-budget controller."""
import subprocess
import tempfile
import unittest
from pathlib import Path

root = Path(__file__).resolve().parents[1]
u32 = 'unsigned int'


class IpcBudget(unittest.TestCase):
    def test_controller_converges_and_clamps(self):
        with tempfile.TemporaryDirectory(prefix='ame-ipc-budget-') as tmp:
            exe = Path(tmp) / 'probe'
            subprocess.run(['clang++', '-std=c++23', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-I' + str(root / 'userland/lib/include'),
                            str(root / 'tests/ipc_budget_probe.cc'), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=30)


if __name__ == '__main__':
    unittest.main()
