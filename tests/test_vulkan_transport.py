from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class VulkanTransport(unittest.TestCase):
    def test_native_transport_end_to_end(self):
        # Compiles the native client (session_client.cc) with the in-memory
        # regression, driving the per-client broker and a reference backend over
        # the wire protocol: session open, host-visible alloc, bounded command
        # record, submit, sync wait, offscreen validation, and the WSI bounded
        # copy + present. Sanitized so an out-of-bounds in the transport fails.
        with tempfile.TemporaryDirectory(prefix='ame-vktransport-') as work:
            probe = str(Path(work) / 'probe')
            subprocess.run(['clang++', '-std=c++23', '-O1', '-g',
                '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-I'+str(ROOT/'shared/include'),
                '-idirafter', str(ROOT/'lib/include'),
                '-I'+str(ROOT/'userland/lib/include'),
                '-I'+str(ROOT/'userland/drivers/gpu/linux'),
                str(ROOT/'tests/vulkan_transport_probe.cc'),
                str(ROOT/'userland/lib/vulkan/session_client.cc'),
                '-o', probe], check=True, capture_output=True)
            result = subprocess.run([probe], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
