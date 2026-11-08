"""Launch AmeFS packages through the production shell without benchmark workers."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(all(shutil.which(name) for name in
                       ('cmake', 'ninja', 'qemu-system-aarch64')),
                     'requires QEMU and freestanding build tools')
class AppPackageSystem(unittest.TestCase):
    def test_amefs_package_launch_and_resources(self):
        build = ROOT / 'out/app-package-smoke'

        def run(command, timeout=180):
            result = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return result

        run(['cmake', '-S', str(ROOT), '-B', str(build), '-G', 'Ninja',
             '-DPython3_EXECUTABLE=' + sys.executable,
             '-DCMAKE_TOOLCHAIN_FILE=' + str(ROOT / 'scripts/build/toolchain-aarch64.cmake'),
             '-DCMAKE_BUILD_TYPE=Release', '-DANKO_BOARD=qemu-virt', '-DANKO_BUILD_DEV=ON',
             '-DDANGO_BUILD_LINUX_ARTIFACTS=OFF'])
        run(['cmake', '--build', str(build), '--target', 'anko',
             'app_package_regressions', '-j', '4'])
        with tempfile.TemporaryDirectory(prefix='ame-package-system-') as work:
            work = Path(work)
            # Cross snapshot-arena boundaries while retaining the demo's UI label.
            sys.path.insert(0, str(ROOT / 'scripts/mx'))
            import mkapppackage
            package = work / 'demo.amp'
            package.write_bytes(mkapppackage.build(
                (build / 'userland/app.package-demo.mxi').read_bytes(),
                {'ui/label.txt': b'Ame packaged UI resources\n',
                 'ui/padding': bytes(range(256)) * 2048}))
            malformed = work / 'bad.amp'
            malformed.write_bytes(package.read_bytes()[:-1])
            disk = work / 'amefs.img'
            run([sys.executable, str(ROOT / 'scripts/mx/mkamefs.py'), 'create',
                 '--build-dir', str(build), '--output', str(disk), '--size', '128M',
                 '--app', 'package-demo.amp=' + str(package),
                 '--app', 'ui-demo.amp=' + str(build / 'userland/ui-demo.amp'),
                 '--app', 'bad-package.amp=' + str(malformed)])
            for cpus, entry_el in ((1, 1), (1, 2), (4, 1), (4, 2)):
                with self.subTest(cpus=cpus, entry_el=entry_el):
                    output = build / f'package-cpu{cpus}-el{entry_el}'
                    result = subprocess.run([
                        sys.executable, str(ROOT / 'scripts/test/boot.py'),
                        '--build', str(build), '--bundle', str(build / 'app-package-system.mxb'),
                        '--cpu', 'cortex-a53', '--cpus', str(cpus), '--entry-el', str(entry_el),
                        '--timeout', '30', '--disk', str(disk),
                        '--marker', 'app package system regression passed',
                        '--output', str(output)], capture_output=True, text=True, timeout=40)
                    serial = (output / 'host.log').read_text()
                    self.assertEqual(result.returncode, 0,
                                     result.stdout + result.stderr + serial[-5000:])
                    self.assertEqual(serial.count('packaged app: resources verified'), 2)
                    self.assertEqual(serial.count('child exited, status=0'), 4)
                    self.assertEqual(serial.count('qml app: bindings, layout, input, resources, raster and idle verified'), 2)
                    self.assertNotIn('dev.apps.installed_bytes=', serial)


if __name__ == '__main__':
    unittest.main()
