from pathlib import Path
import shutil
import platform
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('clang++'), 'requires clang++')
class IdentityMap(unittest.TestCase):
    def test_dop_runs_leaves_ownership_attribute_changes_and_allocation_failures(self):
        with tempfile.TemporaryDirectory(prefix='ame-identity-map-') as directory:
            for board in ('generic', 'anne', 'anne-synthetic'):
                with self.subTest(board=board):
                    program = Path(directory) / board
                    defines = ['-DANKO_HOST_TEST=1']
                    if platform.machine().lower() in ('x86_64', 'amd64'):
                        defines += ['-mcx16']
                    if board != 'generic':
                        defines += ['-DANKO_BOARD_HUAWEI_ANNE=1', '-DANKO_ANNE_FRAMEBUFFER=1']
                    if board == 'anne-synthetic':
                        defines += ['-DANKO_ANNE_DIAG_TEST=1', '-DANKO_ANNE_FRAMEBUFFER=1']
                    build = subprocess.run([
                        'clang++', '-std=c++23', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer', *defines,
                        '-I' + str(ROOT / 'kernel/include'), '-I' + str(ROOT / 'shared/include'),
                        '-I' + str(ROOT / 'boards/huawei-anne'),
                        '-idirafter', str(ROOT / 'lib/include'),
                        str(ROOT / 'tests/identity_map_probe.cc'), str(ROOT / 'lib/libk/string.cc'),
                        '-o', str(program)], capture_output=True, text=True)
                    self.assertEqual(build.returncode, 0, build.stderr)
                    result = subprocess.run([str(program)], capture_output=True, text=True, timeout=15)
                    self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
