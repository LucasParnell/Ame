from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
import panic_bundle  # noqa: E402


def build_probe(work: Path, generate) -> Path:
    compiler = shutil.which('clang++') or shutil.which('g++')
    if not compiler:
        raise unittest.SkipTest('no host C++ compiler')
    include = work / 'generated'
    include.mkdir(parents=True, exist_ok=True)
    subprocess.run([sys.executable, str(ROOT / 'scripts/gen-qr-tables.py'),
                    str(ROOT / 'kernel/assets/qr_tables.json'),
                    str(include / 'qr_tables.h')], check=True, capture_output=True)
    probe = work / 'panic_url_probe'
    subprocess.run([
        compiler, '-std=c++23', '-O1', '-g',
        '-I' + str(ROOT / 'kernel/include'),
        '-I' + str(ROOT / 'shared/include'),
        '-I' + str(include),
        str(ROOT / 'tests/panic_url_probe.cc'),
        str(ROOT / 'kernel/panic_report.cc'),
        str(ROOT / 'kernel/lzma.cc'),
        '-o', str(probe)], check=True, capture_output=True)
    return probe


class PanicUrl(unittest.TestCase):
    """The URL the QR carries must decode back to the exact report."""

    @classmethod
    def setUpClass(cls):
        cls._tmp = tempfile.TemporaryDirectory(prefix='ame-panic-url-')
        cls.work = Path(cls._tmp.name)
        cls.probe = build_probe(cls.work, None)

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def test_url_decodes_to_the_report(self):
        result = subprocess.run([str(self.probe)], capture_output=True, text=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stderr)
        url = result.stdout.strip()
        self.assertTrue(url.startswith('https://'), url)

        bundle = panic_bundle.decode_bundle(url)
        self.assertTrue(bundle['compressed'], 'the panic payload must be LZMA-compressed')
        self.assertLess(bundle['compressed_len'], bundle['raw_len'],
                        'compression must actually shrink the bundle')
        self.assertEqual(bundle['core'], 2)
        self.assertEqual(bundle['reason'], 'data abort (EL1)')
        self.assertEqual(bundle['build'], 'ame-test-1.2.3')
        self.assertEqual(bundle['regs']['esr'], 0x96000045)
        self.assertEqual(bundle['regs']['far'], 0x42)
        self.assertEqual(bundle['regs']['elr'], 0x100018a4)
        self.assertEqual(bundle['regs']['spsr'], 0x600003c5)
        self.assertEqual(bundle['regs']['sp'], 0xffff000000123400)
        self.assertEqual(bundle['regs']['fp'], 0xffff000000123500)
        self.assertEqual(bundle['regs']['lr'], 0x10001f20)
        self.assertEqual(bundle['x'], [0x1000 + i for i in range(8)])
        self.assertEqual(bundle['bt'], [0x10001f20, 0x10001f60, 0x10001fa0])
        self.assertEqual(bundle['virtio'], 4)
        self.assertEqual(bundle['boot_el'], 2)
        self.assertEqual(bundle['pci_base'], 0x10000000)
        self.assertEqual(bundle['pci_size'], 0x2eff0000)
        self.assertEqual(bundle['ram_base'], 0x40000000)
        self.assertEqual(bundle['ram_size'], 0x20000000)
        self.assertEqual(bundle['cap_denials'], 3)

    def test_bundle_fits_a_small_qr(self):
        """The whole point of compressing: a version a camera can read at 3 px."""
        result = subprocess.run([str(self.probe)], capture_output=True, text=True, timeout=120)
        url = result.stdout.strip()
        # Version 1..14 hold 21..73 modules at ECC M; keep the URL well inside.
        self.assertLess(len(url), 400, f'URL is {len(url)} characters')


if __name__ == '__main__':
    unittest.main()
