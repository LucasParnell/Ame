from pathlib import Path
import json
import math
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
import qr_decode  # noqa: E402

try:
    import segno
    HAVE_SEGNO = True
except ImportError:  # pragma: no cover
    HAVE_SEGNO = False


def build_probe(work: Path) -> Path:
    compiler = shutil.which('clang++') or shutil.which('g++')
    if not compiler:
        raise unittest.SkipTest('no host C++ compiler')
    generate = subprocess.run(
        [sys.executable, str(ROOT / 'scripts/gen-qr-tables.py'),
         str(ROOT / 'kernel/assets/qr_tables.json'), str(work / 'qr_tables.h')],
        capture_output=True, text=True)
    if generate.returncode != 0:
        raise AssertionError(generate.stderr)
    probe = work / 'qr_probe'
    subprocess.run([
        compiler, '-std=c++23', '-O1', '-g',
        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
        '-I' + str(ROOT / 'kernel/include'),
        '-I' + str(ROOT / 'shared/include'),
        '-I' + str(work),
        str(ROOT / 'tests/qr_probe.cc'),
        str(ROOT / 'kernel/qr.cc'),
        '-o', str(probe)], check=True, capture_output=True)
    return probe


PAYLOADS = {
    'tiny': b'AME',
    'url': b'https://ame.example/panic?id=0x1f2e3d4c&core=2&trap=data-abort',
    'registers': b' '.join(b'x%d=0x%016x' % (i, i * 0x1111 + 0x1000) for i in range(31)),
    'report': (b'AME PANIC REPORT\n'
               b'trap=data abort (EL1) ec=0x25 iss=0x45\n'
               b'esr=0x96000045 far=0x0000000000000042 elr=0x00000000100018a4\n'
               b'sp=0xffff000000123400\n'
               + b' '.join(b'x%d=0x%016x' % (i, i * 0x1234567) for i in range(31)) + b'\n'
               + b''.join(b'#%d 0x%016x\n' % (i, 0x10001000 + i * 0x40) for i in range(12))),
    'noise': bytes((i * 37 + 11) % 251 + 1 for i in range(900)),
}


class QrEncoder(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tables = qr_decode.load_tables(ROOT / 'kernel/assets/qr_tables.json')
        cls._tmp = tempfile.TemporaryDirectory(prefix='ame-qr-')
        cls.work = Path(cls._tmp.name)
        cls.probe = build_probe(cls.work)

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def capacity(self, version, level):
        return sum(g['blocks'] * g['data'] for g in self.tables['ec'][str(version)][level])

    def expected_version(self, payload_len, level):
        for version in range(1, 41):
            cc = 8 if version <= 9 else 16
            needed = (4 + cc + 8 * payload_len + 7) // 8
            if self.capacity(version, level) >= needed:
                return version
        return None

    def encode(self, payload: bytes, level: str):
        path = self.work / 'payload.bin'
        path.write_bytes(payload)
        result = subprocess.run([str(self.probe), str(path), level],
                                capture_output=True, text=True, timeout=180)
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = result.stdout.strip().splitlines()
        header = dict(part.split('=') for part in lines[0].split())
        rows = lines[1:]
        self.assertEqual(len(rows), int(header['size']))
        for row in rows:
            self.assertEqual(len(row), int(header['size']))
        return int(header['version']), int(header['mask']), rows

    def test_kernel_output_decodes_back_to_the_payload(self):
        for name, payload in PAYLOADS.items():
            for level in ('L', 'M', 'Q', 'H'):
                with self.subTest(payload=name, level=level):
                    version, _mask, rows = self.encode(payload, level)
                    decoded = qr_decode.decode(rows, self.tables)
                    self.assertEqual(decoded['payload'], payload,
                                     f'{name}/{level}: payload round-trip')
                    self.assertEqual(decoded['level'], level, f'{name}/{level}: EC level')
                    self.assertEqual(decoded['version'], version)
                    self.assertTrue(decoded['ec_ok'])

    def test_smallest_fitting_version_is_chosen(self):
        for name, payload in PAYLOADS.items():
            for level in ('L', 'M', 'Q', 'H'):
                with self.subTest(payload=name, level=level):
                    version, _mask, _rows = self.encode(payload, level)
                    self.assertEqual(version, self.expected_version(len(payload), level))

    def test_decoder_agrees_with_the_reference_encoder(self):
        """Guard the guard: the decoder must accept an unrelated encoder's output."""
        if not HAVE_SEGNO:
            self.skipTest('segno not installed in this interpreter')
        for name, payload in PAYLOADS.items():
            with self.subTest(payload=name):
                qr = segno.make(payload, error='M', mode='byte', boost_error=False, micro=False)
                matrix = [[1 if bit else 0 for bit in row] for row in qr.matrix]
                border = (len(matrix) - (17 + 4 * qr.version)) // 2
                if border:
                    matrix = [row[border:len(row) - border]
                              for row in matrix[border:len(matrix) - border]]
                rows = [''.join(str(c) for c in row) for row in matrix]
                decoded = qr_decode.decode(rows, self.tables)
                self.assertEqual(decoded['payload'], payload)
                self.assertEqual(decoded['level'], 'M')

    def test_payload_that_does_not_fit_is_rejected(self):
        path = self.work / 'huge.bin'
        path.write_bytes(b'A' * 3000)
        result = subprocess.run([str(self.probe), str(path), 'H'],
                                capture_output=True, text=True, timeout=180)
        self.assertNotEqual(result.returncode, 0, 'an oversized payload must be rejected')


if __name__ == '__main__':
    unittest.main()
