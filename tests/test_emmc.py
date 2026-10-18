from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('clang++'), 'requires clang++')
class Emmc(unittest.TestCase):
    def test_controller_and_firmware_transport(self):
        with tempfile.TemporaryDirectory(prefix='ame-emmc-') as directory:
            probe = Path(directory) / 'probe'
            build = subprocess.run([
                'clang++', '-std=c++23', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                '-DANKO_HOST_TEST=1', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-I' + str(ROOT / 'kernel/include'), '-I' + str(ROOT / 'shared/include'),
                '-I' + str(ROOT / 'boards/huawei-anne'),
                str(ROOT / 'tests/emmc_probe.cc'),
                str(ROOT / 'shared/drivers/emmc/sdhci.cc'),
                str(ROOT / 'boards/huawei-anne/emmc.cc'), '-o', str(probe)],
                capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            for scenario in ('dma', 'dma-error', 'dma-timeout', 'dma-reset-error', 'blocks', 'cache-flush', 'buffered-flush', 'user128', 'wide', 'wide-fallback', 'wide-restore-error', 'counter', 'card-result', 'write', 'legacy-reliable',
                             'read32', 'invalid', 'data-timeout', 'crc-error', 'r1-error',
                             'restore-error', 'switch-error', 'bad-response', 'bridge',
                             'key-missing', 'key-unknown', 'provision', 'wrong-counter',
                             'power-off', 'clock-timeout', 'reset-timeout', 'command-timeout',
                             'byte-addressing', 'native-4k'):
                with self.subTest(scenario=scenario):
                    run = subprocess.run([str(probe), scenario], capture_output=True, text=True, timeout=10)
                    self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == '__main__':
    unittest.main()
