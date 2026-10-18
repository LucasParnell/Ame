from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MemoryStats(unittest.TestCase):
    def test_reserved_allocated_freed_and_clamped_pages(self):
        with tempfile.TemporaryDirectory(prefix='ame-memory-stats-') as work:
            probe = str(Path(work) / 'probe')
            subprocess.run([
                'clang++', '-std=c++23', '-O1', '-g', '-DANKO_HOST_TEST',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-I' + str(ROOT / 'kernel/include'), '-I' + str(ROOT / 'shared/include'),
                '-idirafter', str(ROOT / 'lib/include'),
                str(ROOT / 'tests/memory_stats_probe.cc'), '-o', probe],
                capture_output=True, text=True, check=True)
            result = subprocess.run([probe], capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)
