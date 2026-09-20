from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class GpuProto(unittest.TestCase):
    def test_header_contract(self):
        with tempfile.TemporaryDirectory(prefix='ame-gpuproto-') as work:
            probe = str(Path(work) / 'probe')
            subprocess.run(['clang++', '-std=c++23', '-O1', '-g',
                '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-I'+str(ROOT/'shared/include'),
                str(ROOT/'tests/gpu_proto_probe.cc'),
                '-o', probe], check=True, capture_output=True)
            result = subprocess.run([probe], capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
