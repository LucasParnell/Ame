from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class UsbCore(unittest.TestCase):
    def test_dwc2_and_hid_report_state_machines(self):
        for source, marker in [('usb_dwc2_probe.cc', 'dwc2 packet probe ok'),
                               ('usb_hid_report_probe.cc', 'hid report probe ok'),
                               ('usb_debug_probe.cc','usb debug probe ok'),
                               ('usb_device_probe.cc','usb device probe ok')]:
            with self.subTest(source=source), tempfile.TemporaryDirectory(prefix='ame-usb-') as work:
                probe = str(Path(work) / 'probe')
                result = subprocess.run([
                    'clang++', '-std=c++23', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I' + str(ROOT / 'tests/mochi_host'),
                    '-I' + str(ROOT / 'shared/include'),
                    '-I' + str(ROOT / 'userland/lib/include'),
                    '-idirafter', str(ROOT / 'lib/include'), str(ROOT / 'tests' / source),
                    '-o', probe], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                result = subprocess.run([probe], capture_output=True, text=True, timeout=20)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn(marker, result.stdout)

    def test_native_dma_owners_have_separate_domains(self):
        with tempfile.TemporaryDirectory(prefix='ame-native-dma-') as work:
            probe = str(Path(work) / 'probe')
            result = subprocess.run([
                'clang++', '-std=c++23', '-DANKO_HOST_TEST', '-O1', '-g',
                '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                '-fno-omit-frame-pointer', '-I' + str(ROOT / 'kernel/include'),
                '-I' + str(ROOT / 'shared/include'), '-idirafter', str(ROOT / 'lib/include'),
                str(ROOT / 'tests/native_dma_probe.cc'), str(ROOT / 'kernel/iommu/native.cc'),
                '-o', probe], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([probe], capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('native DMA isolation probe ok', result.stdout)

    def test_descriptors_transfer_ownership_and_controller_rings(self):
        with tempfile.TemporaryDirectory(prefix='ame-usb-') as work:
            probe = str(Path(work) / 'probe')
            result = subprocess.run([
                'clang++', '-std=c++23', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-I' + str(ROOT / 'shared/include'),
                '-I' + str(ROOT / 'userland/lib/include'),
                '-idirafter', str(ROOT / 'lib/include'),
                str(ROOT / 'tests/usb_core_probe.cc'), '-o', probe],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([probe], capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('usb core probe ok', result.stdout)


if __name__ == '__main__':
    unittest.main()
