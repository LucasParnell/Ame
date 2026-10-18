from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class CounterScale(unittest.TestCase):
    def test_exact_conversion_for_counter_wrap_and_arbitrary_frequencies(self):
        with tempfile.TemporaryDirectory(prefix='ame-counter-scale-') as work:
            binary = str(Path(work) / 'probe')
            subprocess.run(['clang++', '-std=c++23', '-O2', '-g', '-fsanitize=address,undefined',
                            '-fno-sanitize-recover=all', '-idirafter', str(ROOT / 'lib/include'),
                            str(ROOT / 'tests/counter_scale_probe.cc'), '-o', binary], check=True)
            subprocess.run([binary], check=True, timeout=15)
