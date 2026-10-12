import io
import tarfile
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts/mx'))
import mxpack
import signing

class Formats(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='ame-formats-')
        cls.work = Path(cls.tmp.name)
        cls.probe = cls.work / 'probe'
        command = ['clang++', '-std=c++23', '-O1', '-g', '-ffreestanding',
                   '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                   '-I'+str(ROOT/'shared/include'), '-I'+str(ROOT/'kernel/include'),
                   '-I'+str(ROOT/'third_party/monocypher'), '-idirafter', str(ROOT/'lib/include'),
                   '-x', 'c++', str(ROOT/'tests/format_probe.cc'), str(ROOT/'shared/format/bundle.cc'), str(ROOT/'shared/format/archive.cc'),
                   str(ROOT/'shared/format/integrity.cc'), str(ROOT/'shared/format/signature.cc'), str(ROOT/'shared/format/image.cc'), str(ROOT/'kernel/init/fdt.cc'),
                   str(ROOT/'lib/libk/string.cc'), str(ROOT/'third_party/monocypher/monocypher.c'),
                   str(ROOT/'third_party/monocypher/monocypher-ed25519.c'), '-o', str(cls.probe)]
        # The harness needs hosted main; only memory routines require freestanding compilation.
        command.remove('-ffreestanding')
        command.insert(1, '-fno-builtin')
        subprocess.run(command, check=True, capture_output=True)
        cls.key = cls.work / 'pub'
        cls.key.write_bytes(signing.public_bytes(signing.private_key().public_key()))
    @classmethod
    def tearDownClass(cls): cls.tmp.cleanup()
    def check_data(self, mode, data, valid):
        fixture = self.work / 'input'; fixture.write_bytes(data)
        result = subprocess.run([str(self.probe), mode, str(fixture), str(self.key)], capture_output=True)
        self.assertEqual(result.returncode, 0 if valid else 1, result.stderr.decode())
    def bundle(self):
        manifest = self.work / 'manifest.json'; manifest.write_text('{"schema":2,"services":[]}')
        image = self.work / 'init'; image.write_bytes(b'test executable')
        bundle = self.work / 'bundle'
        mxpack.build_bundle(bundle, manifest, ['exec:init:'+str(image)], 'manifest.json')
        return bytearray(bundle.read_bytes())
    def crc(self, b): struct.pack_into('<I', b, 64, mxpack.crc32c_with_zeroed_field(b, 64, 4))
    def test_signed_bundle(self): self.check_data('bundle', self.bundle(), True)
    def test_signature_tamper(self):
        b = self.bundle(); h = struct.unpack_from('<IHHIIQQQQQQIIII48s', b)
        manifest = h[6]; signature = struct.unpack_from('<Q', b, manifest+40)[0]
        b[manifest+signature] ^= 1
        self.crc(b); self.check_data('bundle', b, False)
    def test_substitution_with_recomputed_hashes(self):
        b = self.bundle(); off = 128 + 80
        record = list(struct.unpack_from('<IIQQQ32sIIQ', b, off)); start, size = record[2:4]
        b[start] ^= 1; payload = bytes(b[start:start+size])
        record[5] = hashlib.sha256(payload).digest(); record[6] = mxpack.crc32c(payload)
        struct.pack_into('<IIQQQ32sIIQ', b, off, *record); self.crc(b)
        self.check_data('bundle', b, False)
    def test_wrong_key(self):
        b = self.bundle(); before = self.key.read_bytes()
        try:
            self.key.write_bytes(bytes(32)); self.check_data('bundle', b, False)
        finally: self.key.write_bytes(before)
    def test_truncated_and_overflowed(self):
        b = self.bundle()
        for cut in [0, 1, 127, 128, len(b)-1]: self.check_data('bundle', b[:cut], False)
        struct.pack_into('<Q', b, 16, 2**64-8); self.crc(b); self.check_data('bundle', b, False)
    def test_duplicate_objects(self):
        manifest = self.work/'m'; manifest.write_text('{}'); image=self.work/'i';image.write_bytes(b'x')
        with self.assertRaises(ValueError): mxpack.build_bundle(self.work/'b', manifest, ['exec:a:'+str(image)]*2)
    def test_archive_lookup_bounds(self):
        self.check_data('archive', self.bundle(), True)
        stream = io.BytesIO()
        with tarfile.open(fileobj=stream, mode='w', format=tarfile.USTAR_FORMAT) as archive:
            entry = tarfile.TarInfo('init')
            entry.size = 3
            archive.addfile(entry, io.BytesIO(b'abc'))
        data = stream.getvalue()
        self.check_data('archive', data, True)
        for cut in [0, 4, 511, 512, 514, 1023]:
            self.check_data('archive', data[:cut], False)
        corrupt = bytearray(data)
        corrupt[124] = ord('9')
        self.check_data('archive', corrupt, False)

    def test_json(self):
        for obj in [{}, {'services':[{'name':'a','caps':[]}]}, {'x':'escaped \\" and slash \\'}]:
            self.check_data('json', json.dumps(obj).encode(), True)
        for data in [b'{"a":1,"a":2}', b'{"a":1,"\\u0061":2}', b'{"a":', b'[1,]', b'{"a":01}', b'{}garbage', b'{"a":"unterminated}', b'['*18+b'0'+b']'*18]:
            self.check_data('json', data, False)
    def test_fdt_bounds(self):
        body = struct.pack('>I',1)+b'\0'*4+struct.pack('>II',2,9)
        header = struct.pack('>10I', 0xd00dfeed, 56+len(body), 56, 56+len(body), 40, 17, 16, 0, 0, len(body))
        b = bytearray(header+bytes(16)+body);self.check_data('fdt', b, True)
        for cut in [0, 20, 39, 55, len(b)-1]:self.check_data('fdt', b[:cut], False)
        struct.pack_into('>I', b, 36, 2**32-1);self.check_data('fdt', b, False)

if __name__ == '__main__': unittest.main()
