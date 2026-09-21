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

    def test_c_header_compiles(self):
        # The guest dango-vkd build consumes the same header as C; prove the C
        # view still compiles and its static_asserts hold.
        with tempfile.TemporaryDirectory(prefix='ame-vksession-c-') as work:
            obj = str(Path(work) / 'vksession_c.o')
            subprocess.run(['clang', '-std=c11', '-O1', '-g',
                '-Wall', '-Wextra', '-Werror', '-c',
                '-I'+str(ROOT/'shared/include'),
                str(ROOT/'tests/vulkan_session_c_probe.c'),
                '-o', obj], check=True, capture_output=True)


if __name__ == '__main__':
    unittest.main()
