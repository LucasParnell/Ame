"""Execute production native EGL/GLES and guest decoding with a mock GL backend.
This checks ABI/lifecycle/error handling; it does not establish GPU rendering.
"""
from pathlib import Path
import subprocess
import os
import shutil
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class NativeGles(unittest.TestCase):
    def test_native_api_through_guest_decoder(self):
        with tempfile.TemporaryDirectory() as tmp:
            probe = str(Path(tmp) / 'probe')
            includes = ['shared/include', 'userland/libgraphics/include',
                        'userland/libegl/include', 'userland/libgles/include']
            sources = ['tests/native_gles_probe.cc', 'tests/gles_backend_probe.cc',
                       'userland/libegl/egl.cc', 'userland/libgles/gles.cc']
            subprocess.run(['clang++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra',
                            '-Werror', '-Wno-missing-field-initializers',
                            '-fsanitize=address,undefined', '-DEGL_NO_PLATFORM_SPECIFIC_TYPES=1',
                            *['-I'+str(ROOT / p) for p in includes],
                            *[str(ROOT / p) for p in sources], '-o', probe], check=True)
            result = subprocess.run([probe], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('encoder, decoder and lifecycle probe passed', result.stdout)

    def test_animation_clock_lease(self):
        with tempfile.TemporaryDirectory() as tmp:
            probe = str(Path(tmp) / 'clock-probe')
            subprocess.run(['clang++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined',
                            '-I'+str(ROOT / 'shared/include'),
                            str(ROOT / 'tests/render_clock_probe.cc'), '-o', probe], check=True)
            subprocess.run([probe], check=True, capture_output=True)

    def test_bounded_shader_cache_snapshots(self):
        with tempfile.TemporaryDirectory() as tmp:
            probe = str(Path(tmp) / 'cache-probe')
            subprocess.run(['clang', '-std=c11', '-O1', '-g', '-Wall', '-Wextra',
                            '-Werror', '-fsanitize=address,undefined',
                            '-I'+str(ROOT / 'shared/include'),
                            '-I'+str(ROOT / 'linux-artifacts/panfrost-renderer'),
                            str(ROOT / 'tests/shader_cache_probe.c'), '-o', probe], check=True)
            subprocess.run([probe], check=True, capture_output=True)

    @unittest.skipUnless(shutil.which('dtc') and shutil.which('fdtget') and shutil.which('fdtput'),
                         'requires device-tree tools')
    def test_renderer_growth_cannot_leave_stale_initrd_bounds(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            initrd = root / 'initramfs.cpio'
            initrd.write_bytes(b'x' * 4090)
            env = dict(os.environ, DANGO_LINUX_OUT=tmp, DANGO_PANFROST_HARDWARE='1',
                       DANGO_LINUX_TIMER_HZ='1920000',
                       DANGO_DTS_TEMPLATE=str(ROOT / 'linux-artifacts/dts/dango-linux-panfrost.dts.in'))
            subprocess.run([str(ROOT / 'scripts/linux/build-dtb.sh')], env=env,
                           capture_output=True, check=True)
            tool = ['python3', str(ROOT / 'scripts/linux/update-initrd-dtb.py'), tmp]
            subprocess.run(tool + ['--check'], capture_output=True, check=True)
            initrd.write_bytes(b'x' * 9000)
            stale = subprocess.run(tool + ['--check'], capture_output=True, text=True)
            self.assertNotEqual(stale.returncode, 0)
            self.assertIn('stale DTB initrd end', stale.stderr)
            subprocess.run(tool, capture_output=True, check=True)
            subprocess.run(tool + ['--check'], capture_output=True, check=True)
            dtb = str(root / 'dango-linux.dtb')
            for node, prop, expected in [('/gpu@60000000', 'status', 'okay'),
                                         ('/timer', 'clock-frequency', '1920000')]:
                result = subprocess.check_output(['fdtget', dtb, node, prop], text=True)
                self.assertEqual(result.strip(), expected)
