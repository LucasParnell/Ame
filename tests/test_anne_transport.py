"""Offline protocol checks; requires no p20re checkout or USB device."""
import contextlib
import io
from pathlib import Path
import struct
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts/anne/tools'))
import anne_transport as transport


def reference_crc(data):
    value = 0
    for byte in data:
        value ^= byte << 8
        for _ in range(8):
            value = ((value << 1) ^ (0x1021 if value & 0x8000 else 0)) & 0xffff
    return value


class Port:
    def __init__(self, replies=None, short=False):
        self.replies = iter(replies) if replies is not None else None
        self.short = short
        self.packets = []
        self.reads = 0

    def write(self, packet):
        self.packets.append(packet)
        return len(packet) - int(self.short)

    def flush(self):
        pass

    def read(self, count):
        self.reads += 1
        return next(self.replies) if self.replies is not None else b'\xaa'


class AnneTransport(unittest.TestCase):
    def upload(self, port, data):
        with mock.patch.object(transport.time, 'sleep'), contextlib.redirect_stdout(io.StringIO()):
            transport.upload(port, 'test', data, 0x10000000)

    def test_idt_frames_crc_reassembly_and_sequence_wrap(self):
        self.assertEqual(reference_crc(b'123456789'), 0x31c3)
        data = bytes(range(256)) * 1024 + b'tail'
        port = Port()
        self.upload(port, data)
        self.assertEqual(port.packets[0][:-2], b'\xfe\x00\xff\x01' +
                         struct.pack('>II', len(data), 0x10000000))
        self.assertEqual(b''.join(p[3:-2] for p in port.packets[1:-1]), data)
        for sequence, packet in enumerate(port.packets[1:-1], 1):
            self.assertEqual(packet[:3], bytes((0xda, sequence & 255, (sequence & 255) ^ 255)))
        self.assertEqual(port.packets[-1][:3], b'\xed\x02\xfd')
        for packet in port.packets:
            self.assertEqual(int.from_bytes(packet[-2:], 'big'), reference_crc(packet[:-2]))

    def test_missing_or_negative_data_ack_stops_remaining_upload(self):
        for response in [b'', b'\x55']:
            with self.subTest(response=response):
                port = Port([b'\xaa', response])
                with self.assertRaises(RuntimeError):
                    self.upload(port, bytes(2048))
                self.assertEqual(len(port.packets), 2)

    def test_short_write_stops_before_reading_ack(self):
        port = Port(short=True)
        with self.assertRaisesRegex(RuntimeError, 'short serial write'):
            self.upload(port, b'data')
        self.assertEqual(port.reads, 0)

    def test_factory_status_preserves_info_and_rejects_failed_commands(self):
        factory = transport.Factory('test')
        factory.rd = mock.Mock()
        factory.rd.read.side_effect = [b'INFO FB LockState: LOCKED\0', b'OKAY\0']
        self.assertEqual(factory.status(), b' FB LockState: LOCKED\n')
        factory.rd.read.side_effect = [b'FAILCommand not allowed\0']
        with self.assertRaisesRegex(RuntimeError, 'Command not allowed'):
            factory.status()


if __name__ == '__main__':
    unittest.main()
