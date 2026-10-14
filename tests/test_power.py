from pathlib import Path
import subprocess
import shutil
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PowerCommands(unittest.TestCase):
    def test_registers_and_shell_requests(self):
        for source in ('anne_power_probe.cc', 'msh_power_probe.cc'):
            with self.subTest(source=source), tempfile.TemporaryDirectory(prefix='ame-power-') as work:
                binary = str(Path(work) / 'probe')
                command = ['clang++', '-std=c++23', '-O1', '-Wall', '-Wextra', '-Werror',
                           '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
                command += ['-I' + str(ROOT / p) for p in
                            ('tests/mochi_host', 'userland/lib/include', 'shared/include')]
                command += ['-idirafter', str(ROOT / 'lib/include'), str(ROOT / 'tests' / source), '-o', binary]
                result = subprocess.run(command, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                result = subprocess.run([binary], capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stderr)


@unittest.skipUnless(shutil.which('qemu-system-aarch64') and shutil.which('cmake'), 'requires QEMU and CMake')
class QemuPower(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = ROOT / 'out/power-qemu-dev'
        for command in (['cmake', '--preset', 'qemu-dev', '-B', str(cls.build)],
                        ['cmake', '--build', str(cls.build), '--target', 'anko', 'power_regressions']):
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=120)
            if result.returncode:
                raise RuntimeError(result.stdout[-4000:] + result.stderr[-4000:])

    def transition(self, name):
        # Same signed packaging path as the existing isolated lifecycle images.
        sys.path.insert(0, str(ROOT / 'scripts/mx'))
        import mxpack
        with tempfile.TemporaryDirectory(prefix='ame-power-qemu-') as work:
            work = Path(work)
            manifest = work / 'manifest.json'
            manifest.write_text('{"schema":2,"services":[]}\n')
            bundle = work / 'test.mxb'
            mxpack.build_bundle(bundle, manifest, ['exec:init:' + str(self.build / f'userland/test.{name}.mxi')])
            command = ['qemu-system-aarch64', '-machine', 'virt,virtualization=on,gic-version=2',
                       '-cpu', 'cortex-a53', '-smp', '2', '-m', '128M', '-display', 'none',
                       '-serial', 'stdio', '-monitor', 'none', '-no-reboot',
                       '-kernel', str(self.build / 'kernel/anko.bin'), '-initrd', str(bundle)]
            result = subprocess.run(command, capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout[-4000:] + result.stderr[-2000:])
            self.assertIn('POWER_TEST checks passed; transition requested', result.stdout)
            self.assertNotIn('POWER_TEST FAILED', result.stdout)

    def test_shutdown_exits_qemu(self):
        self.transition('poweroff')

    def test_reboot_exits_qemu_with_no_reboot(self):
        self.transition('reboot')
