"""Exercise the real two-process transports with sanitizers and backpressure."""
import hashlib
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from benchmarks.ipc import results


@unittest.skipUnless(sys.platform == 'linux' and shutil.which('clang++'), 'requires Linux and Clang')
class LinuxIpcBenchmark(unittest.TestCase):
    def test_process_separation_fragment_sizes_partial_windows_and_both_transports(self):
        with tempfile.TemporaryDirectory(prefix='ame-ipc-benchmark-') as directory:
            binary = Path(directory) / 'ipc-linux'
            digest = hashlib.sha256((ROOT / 'benchmarks/ipc/workload.h').read_bytes()).hexdigest()
            subprocess.run(['clang++', '-std=c++23', '-O2', '-g', '-fsanitize=address,undefined',
                            '-fno-sanitize-recover=all', f'-DIPC_BENCH_CONTRACT_SHA256="{digest}"',
                            '-I' + str(ROOT / 'shared/include'), '-idirafter', str(ROOT / 'lib/include'),
                            str(ROOT / 'benchmarks/ipc/linux.cc'), str(ROOT / 'lib/libk/printf.cc'),
                            '-o', str(binary)], check=True, capture_output=True, text=True)
            cpus = sorted(os.sched_getaffinity(0))
            for transport in ('pipe', 'seqpacket'):
                with self.subTest(transport=transport):
                    environment = dict(os.environ)
                    environment['ASAN_OPTIONS'] = 'detect_leaks=0'
                    process = subprocess.Popen([str(binary), transport, str(cpus[0]), str(cpus[-1]),
                                                '33', '5', '32', '3', '16', '31', '1', '1'],
                                               stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                               text=True, env=environment, start_new_session=True)
                    try:
                        stdout, stderr = process.communicate(timeout=30)
                    finally:
                        if process.poll() is None:
                            os.killpg(process.pid, signal.SIGKILL)
                            process.communicate()
                    self.assertEqual(process.returncode, 0, stderr)
                    self.assertEqual(stderr, '')
                    run = results.parse_log(stdout, {'contract_sha256': digest})
                    self.assertEqual(len(run['cases']), 20 if len(cpus) > 1 else 10)
                    self.assertEqual(len(run['skipped']), 10 if len(cpus) > 1 else 20)
                    self.assertNotEqual(run['header']['client_process'], run['header']['server_process'])
                    self.assertTrue(all(row['elapsed_ns'] > 0 for case in run['cases'] for row in case['rounds']))
                    self.assertEqual({case['spec']['payload_bytes'] for case in run['cases']}, {0, 8, 48, 256, 4096})


if __name__ == '__main__':
    unittest.main()
