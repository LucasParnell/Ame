"""The build-time profile generator must emit exactly what the C++ codec reads."""
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

root = Path(__file__).resolve().parents[1]
gen = root / 'scripts/gen-ipc-profile.py'


def fnv1a(data: bytes) -> int:
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


class ProfileGenerator(unittest.TestCase):
    def run_gen(self, text):
        with tempfile.TemporaryDirectory(prefix='ame-ipc-gen-') as tmp:
            conf = Path(tmp) / 'ipc.conf'
            conf.write_text(text)
            out = Path(tmp) / 'blob.bin'
            subprocess.run([sys.executable, str(gen), '--config', str(conf),
                            '--header', str(Path(tmp) / 'blob.h'), '--binary', str(out)], check=True)
            return out.read_bytes()

    def test_roundtrip_matches_codec(self):
        blob = self.run_gen("msh=256\nio.console=0  # explicit\n")
        self.assertEqual(blob[:4], b'MXIP')
        version, count = struct.unpack_from('<HH', blob, 4)
        self.assertEqual((version, count), (1, 2))
        for index in range(count):
            raw = blob[8 + index * 16: 8 + (index + 1) * 16]
            name_hash, budget, flags, check, reserved = struct.unpack('<IHHII', raw)
            self.assertEqual(check, fnv1a(raw[:4] + raw[4:8] + raw[12:16]))
            self.assertLessEqual(budget, 4096)

    def test_rejects_bad_config(self):
        too_many = "".join(f"s{i}=1\n" for i in range(33))
        for text in ("msh=5000\n", "msh\n", "msh=1\nmsh=2\n", too_many):
            with self.subTest(text=text):
                with self.assertRaises(subprocess.CalledProcessError):
                    self.run_gen(text)


if __name__ == '__main__':
    unittest.main()
