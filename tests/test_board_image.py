import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    'board_image', Path(__file__).resolve().parents[1] / 'scripts/check-board-image.py')
board_image = importlib.util.module_from_spec(spec)
spec.loader.exec_module(board_image)


class BoardImageTests(unittest.TestCase):
    def fixture(self, folder, virtual=0x480000, physical=0x480000, entry_delta=0):
        folder = Path(folder)
        (folder / 'kernel').mkdir()
        (folder / 'board.json').write_text(json.dumps({
            'board': 'huawei-anne', 'load_base': '0x480000', 'text_offset': '0x80000'}))
        image = bytearray(64)
        image[56:60] = b'ARM\x64'
        struct.pack_into('<Q', image, 8, 0x80000)
        elf = bytearray(0x140)
        elf[:6] = b'\x7fELF\x02\x01'
        struct.pack_into('<H', elf, 18, 183)
        struct.pack_into('<Q', elf, 24, virtual + entry_delta)
        struct.pack_into('<Q', elf, 32, 64)
        struct.pack_into('<HH', elf, 54, 56, 1)
        struct.pack_into('<II6Q', elf, 64, 1, 5, 0x100, virtual, physical, 64, 64, 0x1000)
        elf[0x100:] = image
        (folder / 'kernel/anko').write_bytes(elf)
        (folder / 'kernel/anko.bin').write_bytes(image)
        return folder

    def test_low_and_high_virtual_entries_map_to_board_physical_entry(self):
        for virtual in (0x480000, 0xffff800000480000):
            with self.subTest(virtual=virtual), tempfile.TemporaryDirectory() as folder:
                self.assertEqual(board_image.check(self.fixture(folder, virtual), 'huawei-anne'),
                                 0x480000)

    def test_wrong_physical_mapping_is_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaisesRegex(ValueError, 'physical entry'):
                board_image.check(self.fixture(folder, physical=0x580000), 'huawei-anne')

    def test_entry_outside_file_backed_code_is_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaisesRegex(ValueError, 'executable load segment'):
                board_image.check(self.fixture(folder, entry_delta=64), 'huawei-anne')

    def test_different_binary_header_is_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            build = self.fixture(folder)
            path = build / 'kernel/anko.bin'
            data = bytearray(path.read_bytes())
            data[0] = 1
            path.write_bytes(data)
            with self.assertRaisesRegex(ValueError, 'ELF entry contents'):
                board_image.check(build, 'huawei-anne')


if __name__ == '__main__':
    unittest.main()
