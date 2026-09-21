from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PanicScreen(unittest.TestCase):
    """The panic-screen logo is a build-generated data asset, and the renderer
    must scale it to fit arbitrary framebuffers without writing out of bounds."""

    def _generate(self, work: Path) -> Path:
        out = work / 'panic_art.h'
        subprocess.run(
            [sys.executable, str(ROOT / 'scripts/mk-ascii-header.py'),
             str(ROOT / 'kernel/assets/panic_art.txt'), str(out), 'anko::art', 'kPanicArt'],
            check=True, capture_output=True)
        return out

    def test_art_is_a_generated_asset_not_hardcoded(self):
        art = ROOT / 'kernel/assets/panic_art.txt'
        self.assertTrue(art.is_file(), 'the logo art asset must exist')
        lines = art.read_text().splitlines()
        while lines and lines[-1] == '':
            lines.pop()
        rows, cols = len(lines), max(len(line) for line in lines)

        # The art must not be pasted into a source file; the kernel only sees
        # the generated header.
        longest = max(lines, key=len).strip()
        self.assertGreater(len(longest), 40)
        for src in list(ROOT.glob('kernel/**/*.cc')) + list(ROOT.glob('kernel/**/*.h')):
            self.assertNotIn(longest, src.read_text(),
                             f'{src} embeds the logo instead of using the generated header')

        with tempfile.TemporaryDirectory(prefix='ame-panic-art-') as tmp:
            header = self._generate(Path(tmp))
            text = header.read_text()
            self.assertIn(f'kPanicArtRows = {rows};', text)
            self.assertIn(f'kPanicArtCols = {cols};', text)

    def test_renderer_scales_and_stays_in_bounds(self):
        with tempfile.TemporaryDirectory(prefix='ame-panic-probe-') as tmp:
            work = Path(tmp)
            header_dir = work / 'generated'
            header_dir.mkdir()
            self._generate(header_dir)
            probe = work / 'panic_screen_probe'
            subprocess.run([
                'clang++', '-std=c++23', '-DANKO_HOST_TEST', '-O1', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-I' + str(ROOT / 'kernel/include'),
                '-I' + str(ROOT / 'shared/include'),
                '-I' + str(header_dir),
                str(ROOT / 'tests/panic_screen_probe.cc'),
                str(ROOT / 'kernel/panic_screen.cc'),
                '-o', str(probe)], check=True, capture_output=True)
            result = subprocess.run([str(probe)], capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('panic screen probe passed', result.stdout)


if __name__ == '__main__':
    unittest.main()
