from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Framebuffer(unittest.TestCase):
    def test_raster_clipping_overlap_and_terminal_backpressure(self):
        for source in ('framebuffer_raster_probe.cc', 'tty_render_probe.cc', 'tty_status_probe.cc'):
            with self.subTest(source=source), tempfile.TemporaryDirectory(prefix='ame-raster-') as work:
                probe = str(Path(work) / 'probe')
                result = subprocess.run([
                    'clang++', '-std=c++23', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I' + str(ROOT / 'tests/mochi_host'),
                    '-I' + str(ROOT / 'userland/lib/include'),
                    '-I' + str(ROOT / 'shared/include'),
                    '-idirafter', str(ROOT / 'lib/include'),
                    str(ROOT / 'tests' / source), '-o', probe], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                result = subprocess.run([probe], capture_output=True, text=True, timeout=20)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_layout_and_bounds(self):
        with tempfile.TemporaryDirectory(prefix='ame-framebuffer-') as work:
            probe = str(Path(work) / 'probe')
            result = subprocess.run([
                'clang++', '-std=c++23', '-O1', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-I' + str(ROOT / 'lib/include'), '-I' + str(ROOT / 'shared/include'),
                str(ROOT / 'tests/framebuffer_probe.cc'), '-o', probe],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([probe], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
