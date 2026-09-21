from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class VulkanSessionAbi(unittest.TestCase):
    def test_cpp_header_contract(self):
        with tempfile.TemporaryDirectory(prefix='ame-vksession-') as work:
            probe = str(Path(work) / 'probe')
            subprocess.run(['clang++', '-std=c++23', '-O1', '-g',
                '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-I'+str(ROOT/'shared/include'),
                str(ROOT/'tests/vulkan_session_probe.cc'),
                '-o', probe], check=True, capture_output=True)
            result = subprocess.run([probe], capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_c_header_and_bounded_decoder(self):
        # The guest dango-vkd build consumes the same header as C; prove the C
        # view compiles, its static_asserts hold and its decoder rejects bad records.
        with tempfile.TemporaryDirectory(prefix='ame-vksession-c-') as work:
            probe = str(Path(work) / 'vksession_c')
            subprocess.run(['clang', '-std=c11', '-O1', '-g',
                '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                '-fno-omit-frame-pointer',
                '-I'+str(ROOT/'shared/include'),
                str(ROOT/'tests/vulkan_session_c_probe.c'),
                '-o', probe], check=True, capture_output=True)
            subprocess.run([probe], check=True, capture_output=True, timeout=20)


if __name__ == '__main__':
    unittest.main()
