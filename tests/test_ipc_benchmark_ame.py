"""End-to-end correctness, including closing a synchronous handoff before exit.

The deliberately short workload is not an accepted performance collection.
"""
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(all(shutil.which(name) for name in
                     ('clang++', 'cmake', 'ninja', 'qemu-system-aarch64', 'ld.lld', 'llvm-objcopy')),
                     'requires QEMU and the freestanding build tools')
class AmeIpcIntegrity(unittest.TestCase):
    def test_records_credits_placements_polling_partial_windows_and_shutdown(self):
        with tempfile.TemporaryDirectory(prefix='ame-ipc-integrity-') as directory:
            output = Path(directory) / 'capture'
            run = subprocess.run([sys.executable, str(ROOT / 'scripts/test/ipc-bench.py'),
                'ame', '--cpu', 'cortex-a53', '--cpus', '4', '--runs', '1',
                '--iterations', '33', '--warmup', '5', '--samples', '32', '--rounds', '3',
                '--window', '16', '--include-spin', '--jobs', '2', '--timeout', '40',
                '--build', str(ROOT / 'out/ipc-bench-smoke'), '--output', str(output)],
                capture_output=True, text=True, timeout=180)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            report = json.loads((output / 'results.json').read_text())
            self.assertEqual(len(report['summary']['cases']), 30)
            for row in report['summary']['cases']:
                self.assertEqual(row['independent_runs'], 1)
                if row['spec']['wait_policy'] == 'blocking':
                    self.assertEqual(row['counters_per_operation']['yields'], 0)
            for serial in output.glob('cpu*-run-*/host.log'):
                self.assertIn('IPC_BENCH_DONE', serial.read_text())
                self.assertNotIn('IPC_BENCH_ERROR', serial.read_text())
