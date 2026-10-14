from pathlib import Path
import importlib.util
import struct
import sys
from unittest import mock
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts/anne'))
SPEC = importlib.util.spec_from_file_location('anne_usb_sender', ROOT / 'scripts/anne/usb-input.py')
sender = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(sender)
import serial_console


class Transport:
    def __init__(self, status=0):
        self.pending = bytearray()
        self.frames = []
        self.status = status

    def write(self, data, deadline):
        sequence, kind, length, crc = struct.unpack_from('<IHHI', data, 4)
        assert data[:4] == b'AMK1' and len(data) == 16 + length
        assert zlib.crc32(data[:12] + data[16:]) == crc
        self.frames.append((sequence, kind, data[16:]))
        ack = sender.encode(sequence, kind | sender.ACK, struct.pack('<i', self.status))
        # Exercise stream resynchronization and a fragmented ACK, not packet-
        # aligned delivery. Serial reads need not match USB packet boundaries.
        self.pending.extend(b'noise' + ack)

    def read(self, deadline):
        data = bytes(self.pending[:3])
        del self.pending[:3]
        return data


class AnneUsbSender(unittest.TestCase):
    def test_raw_console_relay_reads_integer_fds_and_drains_before_exit(self):
        emitted = []
        with mock.patch.object(serial_console.select, 'select', side_effect=[
            ([17], [], []), ([19], [], []), ([], [17], []), ([], [17], [])]), \
             mock.patch.object(serial_console.os, 'read', side_effect=[b'reply', b'help\x1d']), \
             mock.patch.object(serial_console.os, 'write', side_effect=[2, 2]) as write:
            self.assertEqual(serial_console.relay(17, 19, emitted.append), 0)
            self.assertEqual(emitted, [b'reply'])
            self.assertEqual(write.call_count, 2)

    def test_serial_setup_failure_closes_fd_and_bad_duration_is_rejected(self):
        with mock.patch.object(serial_console.os, 'open', return_value=17), \
             mock.patch.object(serial_console.termios, 'tcgetattr', side_effect=OSError('setup')), \
             mock.patch.object(serial_console.os, 'close') as close:
            with self.assertRaises(OSError):
                serial_console.Serial('synthetic')
            close.assert_called_once_with(17)
        for duration in (-1, float('inf'), float('nan')):
            with self.assertRaises(ValueError):
                serial_console.console_mode('synthetic', 'help', True, duration)

    def test_key_sequence_requires_both_press_and_release_ack(self):
        transport = Transport()
        client = sender.Sender(transport)
        client.request(sender.HELLO)
        client.character('A')
        client.character('\n')
        client.request(sender.RELEASE)
        self.assertEqual([f[0] for f in transport.frames], list(range(1, 7)))
        self.assertEqual(transport.frames[1][2], struct.pack('<HBB', 4, 2, 1))
        self.assertEqual(transport.frames[2][2], struct.pack('<HBB', 4, 0, 0))
        self.assertEqual(transport.frames[3][2], struct.pack('<HBB', 0x28, 0, 1))

    def test_rejection_and_unencodable_text_are_explicit(self):
        client = sender.Sender(Transport(status=-2))
        with self.assertRaisesRegex(RuntimeError, 'status -2'):
            client.request(sender.HELLO)
        with self.assertRaises(ValueError):
            client.character('€')
        with self.assertRaises(ValueError):
            sender.encode(0, sender.KEY)
        with self.assertRaises(ValueError):
            sender.encode(1, sender.KEY, b'x' * 65)


if __name__ == '__main__':
    unittest.main()
