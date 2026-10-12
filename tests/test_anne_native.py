import gzip
import hashlib
import importlib.util
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('anne_native_package', ROOT / 'scripts/anne/build-native-package.py')
package = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(package)


def raw_image():
    data = bytearray(128)
    data[64:] = bytes(range(64))
    data[56:60] = b'ARM\x64'
    struct.pack_into('<3Q', data, 8, 0x80000, 4096, 0)
    return bytes(data)


def stock_image():
    raw = raw_image()
    kernel = gzip.compress(raw, mtime=0)
    data = bytearray(24 * 1024**2)
    data[:8] = b'ANDROID!'
    struct.pack_into('<10I', data, 8, len(kernel), 0x480000, 0, 0x8000000,
                     0, 0x1300000, 0x7e00000, 2048, 0, 0x10000129)
    data[576:596] = hashlib.sha1(kernel + struct.pack('<III', len(kernel), 0, 0)).digest()
    data[2048:2048 + len(kernel)] = kernel
    return bytes(data)


class NativePackage(unittest.TestCase):
    def test_stock_avb_footer_uses_observed_effective_partition_end(self):
        data = bytearray(stock_image())
        effective = len(data) - 4096
        data[4096:4100] = b'AVB0'
        struct.pack_into('>I', data, 4096 + 28, 1)
        data[effective - 64:effective] = struct.pack('>4sIIQQQ28s', b'AVBf', 1, 0,
                                                    4096, 4096, 256, bytes(28))
        self.assertEqual(package.footer_layout(data), (effective, 0))
        for field_offset in [effective - 64, 4096 + 120]:
            damaged = bytearray(data)
            damaged[field_offset] ^= 1
            with self.assertRaises(ValueError):
                package.footer_layout(damaged)
        conventional = bytearray(data)
        conventional[-64:] = conventional[effective - 64:effective]
        conventional[effective - 64:effective] = bytes(64)
        with self.assertRaises(ValueError):
            package.footer_layout(conventional)

    def test_visible_probe_rejects_unsafe_scanout(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / 'display-probe')
            subprocess.run(['clang++', '-std=c++23', '-O1', '-fsanitize=address,undefined',
                            '-I' + str(ROOT / 'boards/huawei-anne/entry-probe'),
                            str(ROOT / 'tests/anne_display_probe.cc'), '-o', binary],
                           check=True, capture_output=True, text=True)
            subprocess.run([binary], check=True, capture_output=True, text=True, timeout=10)

    def test_native_container_keeps_measured_header_and_validates_payload(self):
        original = stock_image()
        native = package.kernel_container(original, raw_image())
        self.assertEqual(native[:8], b'ANDROID!')
        self.assertEqual(native[12:576], original[12:576])
        self.assertEqual(native[608:2048], original[608:2048])
        length = struct.unpack_from('<I', native, 8)[0]
        kernel = native[2048:2048 + length]
        self.assertEqual(gzip.decompress(kernel), raw_image())
        self.assertEqual(native[576:596], hashlib.sha1(kernel + struct.pack('<III', length, 0, 0)).digest())
        self.assertEqual(len(native) % 2048, 0)

    def test_rejects_wrong_stock_address_size_and_legacy_checksum(self):
        for offset in [12, 576, 2048]:
            original = bytearray(stock_image())
            original[offset] ^= 1
            with self.assertRaises((ValueError, zlib.error)):
                package.kernel_container(bytes(original), raw_image())
        with self.assertRaises(ValueError):
            package.kernel_container(stock_image()[:-1], raw_image())

    def test_rejects_oversized_or_wrong_offset_native_image(self):
        for offset, footprint in [(0x40000000, 4096), (0x80000, 65 * 1024**2), (0x80000, 64)]:
            image = bytearray(raw_image())
            struct.pack_into('<2Q', image, 8, offset, footprint)
            with self.assertRaises(ValueError):
                package.kernel_container(stock_image(), image)


class NativeLogLayout(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory(prefix='ame-native-log-')
        cls.probe = str(Path(cls.work.name) / 'probe')
        subprocess.run(['clang++', '-std=c++23', '-O1', '-g', '-fsanitize=address,undefined',
                        '-I' + str(ROOT / 'kernel/include'), '-I' + str(ROOT / 'lib/include'),
                        '-I' + str(ROOT / 'boards/huawei-anne'),
                        str(ROOT / 'tests/anne_native_log_probe.cc'),
                        str(ROOT / 'boards/huawei-anne/native_log.cc'),
                        str(ROOT / 'kernel/init/fdt.cc'), str(ROOT / 'lib/libk/string.cc'),
                        '-o', cls.probe], check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def layout(self, base=0x34800000, phandle=1, status='okay', ecc=0):
        dts = f'''/dts-v1/;
        / {{ #address-cells=<2>; #size-cells=<2>;
          reserved-memory {{ #address-cells=<2>; #size-cells=<2>; ranges;
            pstore-mem {{ reg=<0 {base} 0 0x100000>; phandle=<1>; status="{status}"; }};
          }};
          ramoops {{ compatible="ramoops"; memory-region=<{phandle}>;
            console-size=<0 0x80000>; pmsg-size=<0 0x20000>; record-size=<0 0x20000>;
            ftrace-size=<0 0>; ecc-size=<{ecc}>;
          }};
        }};'''
        path = Path(self.work.name) / 'fixture.dtb'
        subprocess.run(['dtc', '-q', '-I', 'dts', '-O', 'dtb', '-o', str(path)],
                       input=dts, capture_output=True, text=True, check=True)
        return subprocess.run([self.probe, str(path)], capture_output=True, text=True,
                              timeout=10, check=True).stdout.strip()

    def test_observed_layout_produces_only_console_region(self):
        self.assertEqual(self.layout(), '1 34860000')

    def test_incompatible_layout_never_selects_physical_memory(self):
        for values in [{'base': 0x35000000}, {'phandle': 2}, {'status': 'disabled'}, {'ecc': 16}]:
            self.assertEqual(self.layout(**values), '0 0')


if __name__ == '__main__':
    unittest.main()
