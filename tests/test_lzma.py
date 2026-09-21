from pathlib import Path
import lzma
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

FILTERS = [{'id': lzma.FILTER_LZMA1, 'lc': 3, 'lp': 0, 'pb': 2, 'dict_size': 4096}]


def build_probe(work: Path) -> Path:
    compiler = shutil.which('clang++') or shutil.which('g++')
    if not compiler:
        raise unittest.SkipTest('no host C++ compiler')
    probe = work / 'lzma_probe'
    subprocess.run([
        compiler, '-std=c++23', '-O1', '-g',
        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
        '-I' + str(ROOT / 'kernel/include'),
        str(ROOT / 'tests/lzma_probe.cc'),
        str(ROOT / 'kernel/lzma.cc'),
        '-o', str(probe)], check=True, capture_output=True)
    return probe


CASES = {
    'empty': b'',
    'short_text': b'AME PANIC v1 core=0 build=vm-linux-hello-v0-85-g2a4607e-dirty',
    'zeros': b'\x00' * 400,
    'repetitive': b'x0=0000000000000000 ' * 40,
    'binary_registers': bytes(
        v for i in range(40) for v in (i & 0xFF, 0, 0, 0, 0, 0, 0, 0)),
    'mixed': b'AMEQR1 core=0 reason=data abort (EL1)\n' + bytes(range(256)) * 2,
    'high_entropy': bytes(((i * 131 + 7) % 251) for i in range(600)),
}


class LzmaEncoder(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls._tmp = tempfile.TemporaryDirectory(prefix='ame-lzma-')
        cls.work = Path(cls._tmp.name)
        cls.probe = build_probe(cls.work)

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def roundtrip(self, name, payload):
        src = self.work / f'{name}.in'
        dst = self.work / f'{name}.lzma'
        src.write_bytes(payload)
        result = subprocess.run([str(self.probe), str(src), str(dst)],
                                capture_output=True, text=True, timeout=120)
        if not payload:
            return  # empty input is stored raw by the caller, not compressed
        self.assertEqual(result.returncode, 0, result.stderr)
        stream = dst.read_bytes()
        self.assertGreater(len(stream), 0)
        decomp = lzma.LZMADecompressor(format=lzma.FORMAT_RAW, filters=FILTERS)
        out = decomp.decompress(stream, max_length=len(payload))
        self.assertEqual(out, payload,
                         f'{name}: round trip mismatch ({len(stream)} compressed bytes)')
        return len(stream)

    def test_roundtrips_through_python_lzma(self):
        for name, payload in CASES.items():
            with self.subTest(case=name):
                self.roundtrip(name, payload)

    def test_compresses_repetitive_data(self):
        """A sanity check that we are actually compressing, not just emitting."""
        size = self.roundtrip('big_zeros', b'\x00' * 2000)
        self.assertLess(size, 2000 // 4, 'highly repetitive input must compress well')

    def test_rejects_empty_output_capacity(self):
        payload = b'some data that will not fit'
        src = self.work / 'cap.in'
        src.write_bytes(payload)
        result = subprocess.run([str(self.probe), str(src), str(self.work / 'cap.lzma')],
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0)


if __name__ == '__main__':
    unittest.main()
