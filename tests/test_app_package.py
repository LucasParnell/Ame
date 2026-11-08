from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts/mx'))
import mkapppackage
import mkamefs


class AppPackage(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='ame-package-')
        cls.work = Path(cls.tmp.name)
        cls.probe = cls.work / 'probe'
        subprocess.run(['clang++', '-std=c++23', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined',
                        '-I' + str(ROOT / 'shared/include'),
                        str(ROOT / 'tests/app_package_probe.cc'), '-o', str(cls.probe)],
                       check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def parse(self, data, valid=True, name=None):
        path = self.work / 'input.amp'
        path.write_bytes(data)
        result = subprocess.run([str(self.probe), str(path)] + ([name] if name else []),
                                capture_output=True)
        self.assertEqual(result.returncode, 0 if valid else 1, result.stderr.decode())
        return result.stdout

    def sample(self):
        # Runtime image validation remains the existing MXI parser's job.
        return mkapppackage.build(b'MXI1executable', {'ui/label.txt': b'label', 'ui/empty': b''})

    def test_round_trip_deterministic_named_and_empty_resources(self):
        data = self.sample()
        self.assertEqual(self.parse(data, name='app.mxi'), b'MXI1executable')
        self.assertEqual(self.parse(data, name='ui/label.txt'), b'label')
        self.assertEqual(self.parse(data, name='ui/empty'), b'')
        self.assertEqual(data, mkapppackage.build(b'MXI1executable',
                         {'ui/empty': b'', 'ui/label.txt': b'label'}))

    def test_cross_arena_and_limit(self):
        payload = bytes(range(256)) * 2048
        data = mkapppackage.build(b'MXI1', {'ui/pixels': payload})
        self.assertEqual(self.parse(data, name='ui/pixels'), payload)
        with self.assertRaises(ValueError):
            mkapppackage.build(b'MXI1', {'ui/huge': bytes(mkapppackage.MAX_BYTES)})

    def test_names_and_entry_limit(self):
        for name in ('', '/absolute', '../escape', 'ui/../escape', 'ui//x',
                     'ui/', 'ui/./x', '.hidden', 'app.mxi', 'ui/é', 'x' * 64, 'a\x00b'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                mkapppackage.build(b'MXI1', {name: b'x'})
        self.parse(mkapppackage.build(b'MXI1', {f'ui/{i}': b'x' for i in range(63)}))
        with self.assertRaises(ValueError):
            mkapppackage.build(b'MXI1', {f'ui/{i}': b'x' for i in range(64)})

    def test_truncation_and_bad_headers(self):
        data = self.sample()
        for cut in range(len(data)):
            self.parse(data[:cut], False)
        for offset, fmt, value in ((0, '<Q', 0), (8, '<I', 2), (12, '<I', 0),
                                   (12, '<I', 65), (16, '<Q', 2**64-1), (24, '<Q', 1),
                                   (96, '<Q', 0), (104, '<Q', 2**64-1)):
            bad = bytearray(data)
            struct.pack_into(fmt, bad, offset, value)
            self.parse(bad, False)
        self.parse(data + b'extra', False)

    def test_duplicate_unsafe_names_and_overlap(self):
        data = self.sample()
        for name in (b'app.mxi', b'ui/../escape', b'ui//x', b'x' * 64):
            bad = bytearray(data)
            bad[112:176] = name.ljust(64, b'\0')
            self.parse(bad, False)
        bad = bytearray(data)
        bad[40] = 1  # nonzero byte after first name terminator
        self.parse(bad, False)
        for value in (0, 2**64-1, len(data)):
            bad = bytearray(data)
            struct.pack_into('<Q', bad, 176, value)
            self.parse(bad, False)

    def test_random_untrusted_input(self):
        rng = random.Random(17)
        for _ in range(40):
            self.parse(rng.randbytes(rng.randrange(1024)), False)

    def test_amefs_offline_provisioning(self):
        build = self.work / 'build'
        (build / 'kernel').mkdir(parents=True, exist_ok=True)
        (build / 'userland').mkdir(exist_ok=True)
        for path in ('kernel/anko', 'userland/manifest.v0', 'manifest.json',
                     'system.mxb', 'userland/test.mxi'):
            (build / path).write_bytes(b'fixture')
        app = self.work / 'demo.amp'
        app.write_bytes(self.sample())
        root, _ = mkamefs.build_tree(build, False, ['demo.amp=' + str(app)])
        data = next(node for node in root.children if node.name == 'data')
        self.assertEqual(data.children[0].name, 'apps')
        self.assertEqual(data.children[0].children[0].src, app.resolve())
        self.assertEqual(data.inode, mkamefs.FS_DATA_ROOT)
        self.assertEqual(data.children[0].parent_inode, data.inode)
        for spec in ('../bad=' + str(app), 'demo=' + str(app) + '=missing'):
            with self.assertRaises((ValueError, FileNotFoundError)):
                mkamefs.build_tree(build, False, [spec])
        with self.assertRaises(ValueError):
            mkamefs.build_tree(build, False, ['demo=' + str(app)] * 2)


if __name__ == '__main__':
    unittest.main()
