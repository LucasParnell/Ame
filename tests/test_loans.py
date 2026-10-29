"""Loan ownership, exact returned tokens, bounds and zero-copy views."""
from pathlib import Path
import subprocess
import tempfile
import unittest
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from benchmarks.ipc.loan_results import parse_log


class Loans(unittest.TestCase):
    def test_ownership_generations_backpressure_and_bounds(self):
        with tempfile.TemporaryDirectory(prefix='ame-loans-') as work:
            binary = str(Path(work) / 'probe')
            subprocess.run(['clang++', '-std=c++23', '-O2', '-g',
                '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                '-I' + str(ROOT / 'userland/lib/include'),
                '-I' + str(ROOT / 'shared/include'), '-idirafter', str(ROOT / 'lib/include'),
                str(ROOT / 'tests/loan_probe.cc'), '-o', binary], check=True)
            subprocess.run([binary], check=True, timeout=15)

    def test_capture_rejects_incomplete_duplicate_and_failed_results(self):
        import json
        header = dict(record='header', schema='ame.ipc-loans/v1', payload_bytes=4096,
                      iterations=33, warmup=5, rounds=3, window=16, buffers=32,
                      server_cpu=0, shared_system=False)
        rows = [header] + [dict(record='round', transport=t, mode=m, round=r,
                               operations=33, elapsed_ns=1000)
                           for t in ('copied_ring', 'shared_loan') for m in ('rpc', 'stream') for r in range(3)]
        def log(records):
            return '\n'.join('IPC_LOAN ' + json.dumps(r) for r in records) + '\nIPC_LOAN_DONE\n'
        self.assertEqual(len(parse_log(log(rows))), 1)
        for invalid in (log(rows[:-1]), log(rows + [rows[-1]]),
                        log(rows) + 'IPC_LOAN_ERROR: corruption\n', log(rows).replace('IPC_LOAN_DONE', '')):
            with self.assertRaises(ValueError):
                parse_log(invalid)
