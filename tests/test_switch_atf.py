from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import switch_atf


class SwitchAtf(unittest.TestCase):
    @staticmethod
    def image(base_instruction=0xd2bffe11):
        data = bytearray(0x1500)
        # Actual installed firmware's reset-handler sequence at 0x14dc,
        # with the MOVZ operand adjusted for the compatible build.
        struct.pack_into('<4I', data, 0x14dc,
                         0x10ff5932, base_instruction, 0xeb11025f, 0x54000260)
        return data

    def test_rejects_installed_wrong_link_address(self):
        with tempfile.TemporaryDirectory(prefix='ame-atf-') as tmp:
            path = Path(tmp) / 'bl31.bin'
            path.write_bytes(self.image(0xd2bff011))  # FF800000, old firmware
            with self.assertRaisesRegex(ValueError, '0xff800000.*0xfff00000'):
                switch_atf.check(path)
            path.write_bytes(self.image())
            self.assertEqual(len(switch_atf.check(path)), 64)

    def test_rejects_literals_and_truncated_or_unknown_code(self):
        for data in (b'', bytes(16), struct.pack('<4I', 0xfff00000, 0, 0, 0),
                     self.image()[:0x14eb]):
            with self.subTest(size=len(data)), self.assertRaises(ValueError):
                switch_atf.linked_base(data)
        data = self.image()
        # Same sequence, but ADR points somewhere other than the image entry.
        struct.pack_into('<I', data, 0x14dc, 0x10000012)
        with self.assertRaises(ValueError):
            switch_atf.linked_base(data)
        data = self.image()
        # A second valid reset handler is ambiguous, even with the same base.
        struct.pack_into('<4I', data, 0, 0x10000012, 0xd2bffe11, 0xeb11025f, 0x54000260)
        with self.assertRaises(ValueError):
            switch_atf.linked_base(data)

    def test_generated_dtb_reserves_actual_firmware_aperture(self):
        with tempfile.TemporaryDirectory(prefix='ame-switch-dtb-') as tmp:
            work = Path(tmp)
            subprocess.run([sys.executable, str(ROOT / 'scripts/switch_atf.py'),
                            'reservations', str(work / 'firmware-reservations.dtsi')], check=True)
            output = work / 'switch.dtb'
            subprocess.run(['dtc', '-q', '-i', str(work), '-I', 'dts', '-O', 'dtb',
                            '-o', str(output), str(ROOT / 'boards/switch-t210/ame-switch.dts')],
                           check=True)
            data = output.read_bytes()
            offset = struct.unpack_from('>I', data, 16)[0]
            ranges = []
            while True:
                base, size = struct.unpack_from('>QQ', data, offset)
                offset += 16
                if not size:
                    break
                ranges.append((base, size))
            # Independently specified from Hekate 6.5.3's SECFW configuration.
            self.assertIn((0xffd00000, 0x300000), ranges)
            self.assertIn((0xa8000000, 0x4000000), ranges)
            self.assertNotIn((0x17c000000, 0x4000000), ranges)

    def test_package_rejects_bad_firmware_before_writing(self):
        with tempfile.TemporaryDirectory(prefix='ame-atf-sd-') as tmp:
            sd = Path(tmp)
            firmware = sd / 'ame/l4t/bl31.bin'
            firmware.parent.mkdir(parents=True)
            firmware.write_bytes(self.image(0xd2bff011))
            result = subprocess.run(['bash', str(ROOT / 'scripts/build-hekate-package.sh'),
                                     '--no-configure', '--no-build', str(sd)],
                                    capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('0xff800000', result.stderr)
            self.assertEqual([p for p in sd.rglob('*') if p.is_file()], [firmware])


if __name__ == '__main__':
    unittest.main()
