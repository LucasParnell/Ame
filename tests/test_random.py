from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]

def blob(seed):
    def word(value): return struct.pack('>I', value)
    def node(name):
        data = word(1)+name.encode()+b'\0'
        return data + bytes(-len(data) % 4)
    strings = b'rng-seed\0'
    body = node('') + node('chosen') + word(3) + word(len(seed)) + word(0) + seed
    body += bytes(-len(seed) % 4) + word(2) + word(2) + word(9)
    return struct.pack('>10I', 0xd00dfeed, 56+len(body)+len(strings), 56,
        56+len(body), 40, 17, 16, 0, len(strings), len(body)) + bytes(16) + body + strings

class Random(unittest.TestCase):
    def test_seed_requirement_reproducibility_and_bounded_output(self):
        with tempfile.TemporaryDirectory(prefix='ame-random-') as directory:
            binary = str(Path(directory)/'probe')
            command = ['clang++', '-std=c++23', '-O1', '-g', '-DANKO_HOST_TEST',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-I'+str(ROOT/'kernel/include'), '-I'+str(ROOT/'shared/include'),
                '-I'+str(ROOT/'third_party/monocypher'), '-idirafter', str(ROOT/'lib/include'),
                '-x', 'c++', str(ROOT/'tests/random_probe.cc'), str(ROOT/'kernel/security/random.cc'),
                str(ROOT/'kernel/init/fdt.cc'), str(ROOT/'lib/libk/string.cc'),
                str(ROOT/'third_party/monocypher/monocypher.c'), '-o', binary]
            built = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            outputs = []
            for seed in (bytes(range(32)), bytes(range(32)), bytes(range(1, 33))):
                fixture = Path(directory)/'seed.dtb'; fixture.write_bytes(blob(seed))
                run = subprocess.run([binary, str(fixture)], capture_output=True, text=True, timeout=20)
                self.assertEqual(run.returncode, 0, run.stderr); outputs.append(run.stdout)
            self.assertEqual(outputs[0], outputs[1]); self.assertNotEqual(outputs[0], outputs[2])
            for seed in (bytes(32), bytes([255])*32, bytes(range(16))):
                fixture.write_bytes(blob(seed))
                run = subprocess.run([binary, str(fixture), 'reject'], capture_output=True, text=True, timeout=20)
                self.assertEqual(run.returncode, 0, run.stderr)

            command.insert(1, '-DANKO_ALLOW_WEAK_ENTROPY=1')
            built = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            fallback = subprocess.run([binary], capture_output=True, text=True, timeout=20)
            self.assertEqual(fallback.returncode, 0, fallback.stderr)
