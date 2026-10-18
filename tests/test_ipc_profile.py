"""Host test for the on-disk IPC profile codec."""
import subprocess
import tempfile
import unittest
from pathlib import Path

root = Path(__file__).resolve().parents[1]


class IpcProfile(unittest.TestCase):
    def test_profile_codec_is_strict(self):
        with tempfile.TemporaryDirectory(prefix='ame-ipc-profile-') as tmp:
            exe = Path(tmp) / 'probe'
            subprocess.run(['clang++', '-std=c++23', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-I' + str(root / 'userland/lib/include'),
                            str(root / 'tests/ipc_profile_probe.cc'), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=30)


if __name__ == '__main__':
    unittest.main()
