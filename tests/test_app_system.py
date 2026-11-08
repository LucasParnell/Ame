"""Repeatedly exec a preinstalled IPC app in the complete A53 system."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from benchmarks.ipc import results


@unittest.skipUnless(all(shutil.which(name) for name in ('cmake', 'ninja', 'qemu-system-aarch64')),
                     'requires QEMU and freestanding build tools')
class AppSystem(unittest.TestCase):
    def test_preinstalled_rejection_repeated_exec_and_both_placements(self):
        build = ROOT / 'out/app-system-smoke'
        configure = ['cmake', '-S', str(ROOT), '-B', str(build), '-G', 'Ninja',
            '-DPython3_EXECUTABLE=' + sys.executable,
            '-DCMAKE_TOOLCHAIN_FILE=' + str(ROOT / 'scripts/build/toolchain-aarch64.cmake'),
            '-DCMAKE_BUILD_TYPE=Release', '-DANKO_BOARD=qemu-virt', '-DANKO_BUILD_DEV=ON',
            '-DDANGO_BUILD_LINUX_ARTIFACTS=OFF',
            '-DAME_IPC_BENCH_ITERATIONS=33', '-DAME_IPC_BENCH_WARMUP=5',
            '-DAME_IPC_BENCH_SAMPLES=32', '-DAME_IPC_BENCH_ROUNDS=3',
            '-DAME_IPC_BENCH_WINDOW=16', '-DAME_IPC_BENCH_SPIN=1']
        with tempfile.TemporaryDirectory(prefix='ame-app-system-') as work:
            for server_cpu, cpus, entry_el in ((0, 1, 1), (0, 1, 2), (0, 4, 1),
                                               (0, 4, 2), (1, 4, 2)):
                with self.subTest(server_cpu=server_cpu, cpus=cpus, entry_el=entry_el):
                    commands = [configure + [f'-DAME_IPC_BENCH_SERVER_CPU={server_cpu}'],
                        ['cmake', '--build', str(build), '--target', 'anko', 'app_system_regressions',
                         'app_terminal_regressions', '-j', '4']]
                    for command in commands:
                        run = subprocess.run(command, capture_output=True, text=True, timeout=180)
                        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                    disk = Path(work) / f'cpu{server_cpu}-smp{cpus}.img'
                    run = subprocess.run([sys.executable, str(ROOT / 'scripts/mx/mkamefs.py'),
                        'create', '--build-dir', str(build), '--output', str(disk), '--size', '128M',
                        '--app', 'package-demo.amp=' + str(build / 'userland/package-demo.amp')],
                        capture_output=True, text=True, timeout=40)
                    self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                    output = Path(work) / f'cpu{server_cpu}-smp{cpus}'
                    run = subprocess.run([sys.executable, str(ROOT / 'scripts/test/boot.py'),
                        '--build', str(build), '--bundle', str(build / 'app-system.mxb'),
                        '--cpu', 'cortex-a53', '--cpus', str(cpus), '--timeout', '30', '--disk', str(disk),
                        '--entry-el', str(entry_el),
                        '--marker', 'app system regression passed', '--output', str(output)],
                        capture_output=True, text=True, timeout=40)
                    serial = (output / 'host.log').read_text()
                    self.assertEqual(run.returncode, 0, run.stdout + run.stderr + serial[-4000:])
                    self.assertNotIn('dev.apps.installed_bytes=', serial)
                    captures = serial.split('APP_SYSTEM_BENCH_BEGIN\n')[1:]
                    self.assertEqual(len(captures), 2)
                    for capture in captures:
                        report = results.parse_log(capture.split('APP_SYSTEM_BENCH_END')[0])
                        self.assertEqual(report['header']['diagnostic_scope'], 'shared_system')
                        self.assertEqual(len(report['cases']), 20 if server_cpu else 10)
                    self.assertEqual(serial.count('child exited, status=0'), 4)
                    self.assertEqual(serial.count('packaged app: resources verified'), 2)
                    terminal_output = Path(work) / f'terminal-cpu{server_cpu}-smp{cpus}'
                    run = subprocess.run([sys.executable, str(ROOT / 'scripts/test/boot.py'),
                        '--build', str(build), '--bundle', str(build / 'app-terminal.mxb'),
                        '--cpu', 'cortex-a53', '--cpus', str(cpus), '--timeout', '40', '--disk', str(disk),
                        '--entry-el', str(entry_el),
                        '--marker', 'app terminal regression passed', '--output', str(terminal_output)],
                        capture_output=True, text=True, timeout=50)
                    serial = (terminal_output / 'host.log').read_text()
                    self.assertEqual(run.returncode, 0, run.stdout + run.stderr + serial[-4000:])
                    captures = serial.split('IPC_BENCH_DONE')
                    self.assertEqual(len(captures), 3)
                    for capture in captures[:2]:
                        report = results.parse_log(capture + 'IPC_BENCH_DONE\n')
                        self.assertEqual(len(report['cases']), 20 if server_cpu else 10)
                    self.assertEqual(serial.count('child exited, status=0'), 2)
                    self.assertEqual(serial.count('\nAPP_TERMINAL_INPUT_WORKS\nmsh:/$ '), 2)
