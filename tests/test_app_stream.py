from pathlib import Path
import importlib.util
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts/anne'))
spec = importlib.util.spec_from_file_location('app_stream', ROOT / 'scripts/anne/app-stream.py')
sender = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sender)

class Transport:
    def __init__(self):
        self.pending = bytearray()
        self.state, self.received, self.code = sender.READY, 0, sender.NO_EXIT
        self.session, self.data, self.types = 123, bytearray(), []
        self.bad_offset = False

    def write(self, data, deadline):
        seq, kind, length, _ = struct.unpack_from('<IHHI', data, 4)
        assert data == sender.encode(seq, kind, data[16:])
        assert length == len(data) - 16
        payload = data[16:]
        if kind != sender.HELLO:
            assert struct.unpack_from('<Q', payload)[0] == self.session
            payload = payload[8:]
        self.types.append(kind)
        if kind == sender.BEGIN:
            self.size, _ = struct.unpack('<II', payload)
            self.state = sender.RECEIVING
        elif kind == sender.DATA:
            offset, = struct.unpack_from('<I', payload)
            assert offset == self.received
            self.data.extend(payload[4:])
            self.received += len(payload) - 4
        elif kind == sender.RUN:
            assert len(self.data) == self.size
            self.state = sender.RUNNING
        elif kind == sender.POLL:
            self.state, self.code = sender.READY, 42
        elif kind == sender.CANCEL:
            self.data.clear()
            self.state = sender.READY
        reply = struct.pack('<iIIiQ', 0, self.state,
                            self.received + int(self.bad_offset and kind == sender.DATA),
                            self.code, self.session)
        self.pending.extend(b'noise' + sender.encode(seq, kind | sender.ACK, reply))

    def read(self, deadline):
        data = bytes(self.pending[:3])
        del self.pending[:3]
        return data

class AppStream(unittest.TestCase):
    def test_session_lifecycle_under_sanitizers(self):
        with tempfile.TemporaryDirectory(prefix='ame-app-stream-') as work:
            output = str(Path(work) / 'probe')
            result = subprocess.run(['clang++', '-std=c++23', '-O1', '-g',
                '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                '-I' + str(ROOT / 'shared/include'), '-idirafter', str(ROOT / 'lib/include'),
                str(ROOT / 'tests/app_stream_probe.cc'), '-o', output], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([output], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('app streaming session probe ok', result.stdout)

    def test_usb_routing_under_backpressure(self):
        with tempfile.TemporaryDirectory(prefix='ame-app-router-') as work:
            output = str(Path(work) / 'probe')
            result = subprocess.run(['clang++', '-std=c++23', '-O1', '-g',
                '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                '-I' + str(ROOT / 'tests/mochi_host'),
                '-I' + str(ROOT / 'shared/include'),
                '-I' + str(ROOT / 'userland/lib/include'), '-idirafter', str(ROOT / 'lib/include'),
                str(ROOT / 'tests/app_router_probe.cc'), str(ROOT / 'lib/libk/printf.cc'),
                '-o', output], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([output], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('app USB router probe ok', result.stdout)

    def test_fragmented_replies_upload_and_exit(self):
        transport = Transport()
        image = b'MXI1' + bytes(range(128))
        self.assertEqual(sender.Sender(transport).run(image), 42)
        self.assertEqual(transport.types.count(sender.DATA), 3)
        self.assertEqual(transport.types[-1], sender.CANCEL)
        self.assertEqual(transport.data, b'')

    def test_bad_offset_cancels_the_upload(self):
        transport = Transport()
        transport.bad_offset = True
        with self.assertRaisesRegex(RuntimeError, 'unexpected offset'):
            sender.Sender(transport).run(b'MXI1' + bytes(124))
        self.assertEqual(transport.types[-1], sender.CANCEL)
        self.assertEqual(transport.data, b'')

    def test_bad_image_does_not_touch_usb(self):
        transport = Transport()
        with self.assertRaises(ValueError):
            sender.Sender(transport).run(b'ELF')
        self.assertEqual(transport.types, [])
