from pathlib import Path
import importlib.util
import struct
import sys
from unittest import mock
import unittest
import zlib
import errno
import os
import pty
import select
import termios

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
    def test_serial_close_does_not_echo_peer_prompt_back_as_commands(self):
        master, slave = pty.openpty()
        try:
            original = termios.tcgetattr(slave)
            original[3] |= termios.ECHO | termios.ECHONL
            termios.tcsetattr(slave, termios.TCSANOW, original)
            link = serial_console.Serial(os.ttyname(slave))
            link.close()
            expected = original[:]
            expected[3] &= ~(termios.ECHO | termios.ECHONL)
            self.assertEqual(termios.tcgetattr(slave), expected)
            os.write(master, b'msh:/$ \r\n')
            self.assertFalse(select.select([master], [], [], .05)[0])
        finally:
            os.close(slave)
            os.close(master)

    def test_interactive_console_reopens_without_replaying_pending_input(self):
        first, second = mock.Mock(fd=17), mock.Mock(fd=23)
        stdin = mock.Mock()
        stdin.isatty.return_value = True
        stdin.fileno.return_value = 19
        stdout = mock.Mock()
        with mock.patch.object(serial_console, 'Serial', side_effect=[first, second]) as opened, \
             mock.patch.object(serial_console.sys, 'stdin', stdin), \
             mock.patch.object(serial_console.sys, 'stdout', stdout), \
             mock.patch.object(serial_console.termios, 'tcgetattr'), \
             mock.patch.object(serial_console.termios, 'tcsetattr'), \
             mock.patch.object(serial_console.tty, 'setraw'), \
             mock.patch.object(serial_console.select, 'select', side_effect=[
                 ([19], [], []), ([], [17], []), ([], [], []), ([19], [], [])]), \
             mock.patch.object(serial_console.os, 'read', side_effect=[b'partial-command', b'\x1d']), \
             mock.patch.object(serial_console.os, 'write', side_effect=OSError(errno.ENXIO, 'lost')) as write:
            # Use the real relay, including a partially queued command when
            # the first fd vanishes. The reconnect must start with no bytes.
            self.assertEqual(serial_console.console_mode('same-phone', None, False, 3), 0)
            self.assertEqual(opened.call_count, 2)
            self.assertEqual(write.call_count, 1)
            first.close.assert_called_once()
            second.close.assert_called_once()

    def test_disconnect_cleanup_is_idempotent_and_does_not_mask_error(self):
        link = serial_console.Serial.__new__(serial_console.Serial)
        link.fd, link.previous = 17, [0] * 7
        with mock.patch.object(serial_console.termios, 'tcsetattr',
                               side_effect=serial_console.termios.error(errno.ENXIO, 'disconnected')), \
             mock.patch.object(serial_console.os, 'close') as close:
            link.close()
            link.close()
            close.assert_called_once_with(17)

    def test_relay_signals_disconnect_for_reconnection(self):
        with mock.patch.object(serial_console.select, 'select', return_value=([17], [], [])), \
             mock.patch.object(serial_console.os, 'read', return_value=b''):
            with self.assertRaises(OSError) as error:
                serial_console.relay(17, 19, lambda _: None)
            self.assertEqual(error.exception.errno, errno.ENODEV)

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
