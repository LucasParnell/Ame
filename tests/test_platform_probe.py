from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('dtc') and shutil.which('clang++'), 'requires dtc and clang++')
class PlatformProbe(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory(prefix='ame-platform-probe-')
        cls.path = Path(cls.work.name)
        cls.probe = cls.path / 'probe'
        subprocess.run([
            'clang++', '-std=c++23', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
            '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
            '-I' + str(ROOT / 'kernel/include'), '-I' + str(ROOT / 'shared/include'),
            '-idirafter', str(ROOT / 'lib/include'),
            str(ROOT / 'tests/platform_probe.cc'), str(ROOT / 'kernel/platform/probe.cc'),
            str(ROOT / 'kernel/init/fdt.cc'), str(ROOT / 'lib/libk/string.cc'),
            '-o', str(cls.probe)], check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def dtb(self, nodes, memreserve=''):
        source = self.path / 'input.dts'
        source.write_text('/dts-v1/; ' + memreserve +
                          ' / { #address-cells=<2>; #size-cells=<2>; ' + nodes + ' };')
        path = self.path / 'input.dtb'
        subprocess.run(['dtc', '-I', 'dts', '-O', 'dtb', '-o', str(path), str(source)],
                       check=True, capture_output=True)
        return path.read_bytes()

    def decode(self, data):
        path = self.path / 'decode.dtb'
        path.write_bytes(data)
        result = subprocess.run([str(self.probe), str(path)], check=True, capture_output=True,
                                text=True, timeout=5)
        return [int(value, 16 if i in (1, 2, 3, 5, 6, 8, 10) else 10)
                for i, value in enumerate(result.stdout.split())]

    def reservations(self, data):
        path = self.path / 'reservations.dtb'
        path.write_bytes(data)
        result = subprocess.run([str(self.probe), str(path), 'reservations'], check=True,
                                capture_output=True, text=True, timeout=5)
        lines = result.stdout.splitlines()
        return ([int(value) for value in lines[0].split()],
                [tuple(int(value, 16) for value in line.split()) for line in lines[1:]])

    def test_reservations_preserve_header_order_and_full_reg_arrays(self):
        # More than the hardware slot's 64-byte reg view, plus partial trailing
        # cells, zero tuples, a 64-bit address, and an unrelated sibling's reg.
        tuples = ' '.join('0 0x%x 0 0x1001' % (0x10000 + i * 0x2000) for i in range(6))
        data = self.dtb('reserved-memory { #address-cells=<2>; #size-cells=<2>; ranges;'
                        ' a { reg=<' + tuples + ' 0 0 0 1 0 0x70000 0 0 1 0 0 0x1000 7>; };'
                        '}; sibling { reg=<0 0x90000 0 0x1000>; };',
                        '/memreserve/ 0 0x1000; /memreserve/ 0x8000 0x2000;')
        header, ranges = self.reservations(data)
        self.assertEqual(header, [1, 1, 9, 2, 0])
        self.assertEqual(ranges, [(0, 0x1000), (0x8000, 0x2000)] +
                         [(0x10000 + i * 0x2000, 0x1001) for i in range(6)] +
                         [(1 << 32, 0x1000)])

    def test_reservations_include_disabled_and_nested_nodes(self):
        data = self.dtb('''
            reserved-memory { status="disabled";
                a { reg=<0 0x10000 0 0x2000>;
                    b { status="disabled"; reg=<0 0x11000 0 0x3000>; }; };
                c { reg=<0 0x40000 0 0x1000>; };
            };
            unrelated { reserved-memory { a { reg=<0 0x90000 0 0x1000>; }; }; };
        ''')
        self.assertEqual(self.reservations(data),
                         ([1, 1, 3, 0, 0], [(0x10000, 0x2000), (0x11000, 0x3000),
                                            (0x40000, 0x1000)]))

    def test_reservation_depth_bound_matches_original_consumer(self):
        nested = 'leaf { reg=<0 0x20000 0 0x1000>; };'
        for i in range(13):
            nested = 'n%d { %s };' % (i, nested)
        data = self.dtb('reserved-memory { a { reg=<0 0x10000 0 0x1000>; ' + nested + '}; };')
        self.assertEqual(self.reservations(data), ([1, 1, 1, 0, 0], [(0x10000, 0x1000)]))

    def test_reservation_capacity_boundary_and_overflow_revoke_publication(self):
        for count in (128, 129):
            nodes = ''.join('n%d { reg=<0 0x%x 0 0x1000>; };' %
                            (i, 0x10000 + i * 0x1000) for i in range(count))
            header, ranges = self.reservations(self.dtb('reserved-memory {' + nodes + '};'))
            self.assertEqual(header, [1, int(count == 128), 128, 0, int(count > 128)])
            self.assertEqual(len(ranges), 128 if count == 128 else 0)

    def test_malformed_reservation_header_keeps_hardware_but_forces_fallback(self):
        data = bytearray(self.dtb('memory@40000000 { reg=<0 0x40000000 0 0x8000000>; };',
                                 '/memreserve/ 0x8000 0x1000;'))
        offset = struct.unpack_from('>I', data, 16)[0]
        struct.pack_into('>QQ', data, offset, (1 << 64) - 1, 0x1000)
        header, ranges = self.reservations(data)
        self.assertEqual((header[0:2], ranges), ([1, 0], []))
        self.assertEqual(self.decode(data)[1:3], [0x40000000, 0x8000000])

    def test_bad_structure_never_publishes_collected_reservations(self):
        data = bytearray(self.dtb('reserved-memory { a { reg=<0 0x10000 0 0x1000>; }; };'))
        offset, size = struct.unpack_from('>I', data, 8)[0], struct.unpack_from('>I', data, 36)[0]
        struct.pack_into('>I', data, offset + size - 4, 0xdeadbeef)
        header, ranges = self.reservations(data)
        self.assertEqual(header[:3], [0, 0, 1])
        self.assertEqual(ranges, [])

    def test_console_replay_does_not_duplicate_reservations(self):
        nodes = ''.join('uart@%x { compatible="arm,pl011"; reg=<0 0x%x 0 0x1000>; };' %
                        (0x1000 + i * 0x1000, 0x1000 + i * 0x1000) for i in range(24))
        data = self.dtb(nodes + 'chosen { stdout-path="/uart@18000"; };'
                        'reserved-memory { a { reg=<0 0x40000 0 0x1000>; }; };')
        self.assertEqual(self.reservations(data), ([1, 1, 1, 0, 0], [(0x40000, 0x1000)]))

    def test_alias_property_order_read_only_views_and_uart_path_after_children(self):
        data = self.dtb('''
            chosen { stdout-path="serial1:115200n8"; };
            aliases { serial1="/bus/uart@2000"; };
            memory@40000000 { reg=<0 0x40000000 0 0x8000000>; };
            bus {
                uart@1000 { compatible="arm,pl011"; reg=<0 0x1000 0 0x1000>; };
                uart@2000 { reg=<0 0x2000 0 0x1000>; compatible="unknown", "arm,pl011";
                            child { value=<1>; }; };
            };
            virtio@3000 { reg=<0 0x3000 0 0x1000>; compatible="virtio,mmio"; };
            gic@4000 { reg=<0 0x4000 0 0x1000 0 0x5000 0 0x1000>; compatible="arm,gic-400"; };
            usb@ff100000 { reg=<0 0xff100000 0 0x40000>; compatible="hisilicon,hi6250-usb-otg"; };
        ''')
        self.assertEqual(self.decode(data)[:9], [1, 0x40000000, 0x8000000, 0x2000,
                                               1, 0x3000, 0x4000, 1, 0x5000])

    def test_disabled_ancestors_and_memory_controller_are_ignored(self):
        data = self.dtb('''
            memory-controller@1000 { reg=<0 0x1000 0 0x1000>; };
            disabled { status="disabled";
                virtio@2000 { status="okay"; compatible="virtio,mmio"; reg=<0 0x2000 0 0x1000>; };
            };
            virtio@3000 { compatible="virtio,mmio"; reg=<0 0x3000 0 0x1000>; status="disabled"; };
        ''')
        result = self.decode(data)
        self.assertEqual(result[:3], [1, 0x40000000, 0x4000000])
        self.assertEqual(result[4], 0)

    def test_bounded_deep_paths_and_sibling_restoration(self):
        nested = 'uart@6000 { compatible="arm,pl011"; reg=<0 0x6000 0 0x1000>; };'
        for i in reversed(range(12)):
            nested = 'n' + str(i) + ' { ' + nested + ' };'
        target = '/' + '/'.join('n' + str(i) for i in range(12)) + '/uart@6000'
        data = self.dtb('chosen { stdout-path="' + target + '"; };' + nested +
                        ' sibling { uart@7000 { compatible="arm,pl011"; reg=<0 0x7000 0 0x1000>; }; };')
        self.assertEqual(self.decode(data)[3], 0x6000)

    def test_malformed_and_truncated_data_never_publish_partial_result(self):
        data = self.dtb('virtio@3000 { compatible="virtio,mmio"; reg=<0 0x3000 0 0x1000>; };')
        for cut in (1, 20, 39, len(data)-1):
            with self.subTest(cut=cut):
                self.assertEqual(self.decode(data[:cut])[0], 0)
        corrupt = bytearray(data)
        off = struct.unpack_from('>I', corrupt, 8)[0]
        size = struct.unpack_from('>I', corrupt, 36)[0]
        struct.pack_into('>I', corrupt, off + size - 4, 0xdeadbeef)
        self.assertEqual(self.decode(corrupt)[0], 0)

    def test_compatibility_priority_and_unterminated_property(self):
        data = self.dtb('device@3000 { compatible="virtio,mmio", "arm,smmu-v3"; reg=<0 0x3000 0 0x1000>; };')
        result = self.decode(data)
        self.assertEqual((result[4], result[9]), (1, 0))
        malformed = self.dtb('device@3000 { compatible=[76 69 72 74 69 6f 2c 6d 6d 69 6f]; reg=<0 0x3000 0 0x1000>; };')
        self.assertEqual(self.decode(malformed)[4], 0)

    def test_path_overflow_does_not_select_a_truncated_name(self):
        parent, leaf = 'a' * 240, 'uart@' + '1' * 20
        truncated = ('/' + parent + '/' + leaf)[:255]
        data = self.dtb('chosen { stdout-path="' + truncated + '"; };' + parent +
                        ' { ' + leaf + ' { compatible="arm,pl011"; reg=<0 0x7000 0 0x1000>; }; };')
        self.assertEqual(self.decode(data)[3], 0xdead0000)

    def test_selection_after_devices_and_alias_before_chosen(self):
        data = self.dtb('''
            bus { uart@1000 { compatible="arm,pl011"; reg=<0 0x1000 0 0x1000>; };
                  uart@2000 { compatible="arm,pl011"; reg=<0 0x2000 0 0x1000>; }; };
            aliases { serial1="/bus/uart@2000"; };
            chosen { stdout-path="serial1:115200n8"; };
        ''')
        self.assertEqual(self.decode(data)[3], 0x2000)

    def test_console_and_alias_overflow_replay_preserves_transports(self):
        nodes = ''.join('uart@%x { compatible="arm,pl011"; reg=<0 0x%x 0 0x1000>; };' %
                        (0x1000 + i * 0x1000, 0x1000 + i * 0x1000) for i in range(24))
        aliases = ''.join('serial%d="/uart@%x";' % (i, 0x1000 + i * 0x1000)
                          for i in range(40))
        data = self.dtb(nodes + 'aliases {' + aliases + '}; chosen { stdout-path="serial23"; };'
                        'virtio@90000 { compatible="virtio,mmio"; reg=<0 0x90000 0 0x1000>; };')
        result = self.decode(data)
        self.assertEqual((result[3], result[4], result[5]), (0x18000, 1, 0x90000))

    def test_console_path_pool_overflow_replays_exact_path(self):
        parent = 'p' * 200
        nodes = ''.join('uart@%x { compatible="arm,pl011"; reg=<0 0x%x 0 0x1000>; };' %
                        (0x1000 + i * 0x1000, 0x1000 + i * 0x1000) for i in range(12))
        data = self.dtb(parent + ' {' + nodes + '}; chosen { stdout-path="/' + parent +
                        '/uart@c000"; };')
        self.assertEqual(self.decode(data)[3], 0xc000)


if __name__ == '__main__':
    unittest.main()
