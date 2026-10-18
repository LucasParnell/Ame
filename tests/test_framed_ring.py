"""Production copied framing: ownership, invalid input, wrap and concurrency."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class FramedRing(unittest.TestCase):
    def test_fragments_backpressure_invalid_input_and_concurrent_wrap(self):
        self.run_probe('framed_ring_probe.cc')

    def test_blocking_wakeup_race_across_single_multi_and_inbox_waits(self):
        self.run_probe('ring_blocking_probe.cc')

    def test_packed_records_block_for_data_and_credits_without_yielding(self):
        self.run_probe('packed_ring_blocking_probe.cc')

    def run_probe(self, source):
        with tempfile.TemporaryDirectory(prefix='ame-framed-ring-') as work:
            binary = str(Path(work) / 'probe')
            subprocess.run(['clang++', '-std=c++23', '-O2', '-g', '-pthread',
                '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                '-I' + str(ROOT / 'tests/mochi_host'), '-I' + str(ROOT / 'userland/lib/include'),
                '-I' + str(ROOT / 'shared/include'), '-idirafter', str(ROOT / 'lib/include'),
                str(ROOT / 'tests' / source), '-o', binary], check=True)
            subprocess.run([binary], check=True, timeout=30)
