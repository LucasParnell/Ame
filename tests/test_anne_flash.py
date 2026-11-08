import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock
from types import SimpleNamespace

SCRIPTS = Path(__file__).resolve().parents[1] / 'scripts/anne/tools'
sys.path.insert(0, str(SCRIPTS))
spec = importlib.util.spec_from_file_location('anne_flash', SCRIPTS / 'flash-el2.py')
flash = importlib.util.module_from_spec(spec)
spec.loader.exec_module(flash)


class FakeFactory:
    def __init__(self, image, corrupt=False, wrong_size=False):
        self.image = image
        self.corrupt = corrupt
        self.wrong_size = wrong_size
        self.commands = []
        self.reads = []
        self.wr = self
        self.rd = self
        self.flashed = False

    def command(self, command):
        self.commands.append(command)
        if command == 'getvar:emmc:kernel':
            size = len(self.image) + int(self.wrong_size)
            return f'{0x100000:016x}:{size:016x}'.encode()
        if command == 'flash:kernel':
            self.flashed = True
        return b''

    def read(self, *args, **kwargs):
        if len(args) == 1:  # download response
            return f'DATA{len(self.image):08x}'.encode()
        kind, address, size = args
        self.reads.append((self.flashed, kind, address, size))
        if not self.flashed or self.corrupt:
            return bytes(size)
        offset = address - 0x100000
        return self.image[offset:offset + size]

    def write(self, data, **kwargs):
        return len(data)

    def status(self):
        return b''


class AnneFlash(unittest.TestCase):
    def test_expected_image_change_stops_before_usb(self):
        with mock.patch.object(sys, 'argv', ['flash-el2', '--package', '/unused',
                                             '--expect-sha256', '0' * 64]), \
             mock.patch.object(flash, 'inputs', return_value=(b'changed image', {}, {})), \
             mock.patch.object(flash, 'enter_lk') as enter, \
             mock.patch.object(flash, 'Factory') as factory:
            with self.assertRaisesRegex(RuntimeError, 'changed after selection'):
                flash.main()
        enter.assert_not_called()
        factory.assert_not_called()

    def test_bootrom_waits_for_tty_creation_and_udev_permissions(self):
        port = SimpleNamespace(vid=0x12d1, pid=0x3609, device='/dev/ttyUSB0')
        discover = mock.Mock(side_effect=[[], [port], [port]])
        with mock.patch.object(flash.os, 'access', side_effect=[False, True]), \
             mock.patch.object(flash.time, 'monotonic', side_effect=[0, 0, 1]), \
             mock.patch.object(flash.time, 'sleep') as sleep:
            self.assertEqual(flash.wait_bootrom_tty(discover), '/dev/ttyUSB0')
            self.assertEqual(sleep.call_count, 2)

    def test_bootrom_ambiguity_and_inaccessible_timeout_stop_before_upload(self):
        port = SimpleNamespace(vid=0x12d1, pid=0x3609, device='/dev/ttyUSB0')
        with self.assertRaisesRegex(RuntimeError, 'Multiple'):
            flash.wait_bootrom_tty(lambda: [port, port])
        with mock.patch.object(flash.os, 'access', return_value=False), \
             mock.patch.object(flash.time, 'monotonic', side_effect=[0, 16]):
            with self.assertRaisesRegex(RuntimeError, 'accessible'):
                flash.wait_bootrom_tty(lambda: [port])

    def test_default_skips_old_kernel_and_verifies_flash(self):
        image = b'new kernel' * 64
        fb = FakeFactory(image)
        report = {}
        with tempfile.TemporaryDirectory() as work:
            flash.install(fb, image, Path(work), report, lambda: None)
            self.assertFalse((Path(work) / 'kernel-before.img').exists())
            self.assertEqual((Path(work) / 'kernel-after.img').read_bytes(), image)
        self.assertTrue(all(after_flash for after_flash, *_ in fb.reads))
        self.assertTrue(report['readback_verified'])

    def test_backup_is_opt_in(self):
        image = b'new kernel' * 64
        fb = FakeFactory(image)
        with tempfile.TemporaryDirectory() as work:
            flash.install(fb, image, Path(work), {}, lambda: None, backup=True)
            self.assertEqual((Path(work) / 'kernel-before.img').stat().st_size, len(image))
        self.assertFalse(fb.reads[0][0])
        self.assertTrue(fb.reads[-1][0])

    def test_explicit_skip_readback_still_flashes_and_records_skip(self):
        image = b'new kernel' * 64
        fb, report = FakeFactory(image), {}
        with tempfile.TemporaryDirectory() as work:
            flash.install(fb, image, Path(work), report, lambda: None,
                          verify_readback=False)
            self.assertFalse((Path(work) / 'kernel-after.img').exists())
        self.assertTrue(fb.flashed)
        self.assertFalse(fb.reads)
        self.assertTrue(report['readback_skipped_by_user'])
        self.assertNotIn('readback_verified', report)

    @unittest.skipUnless(importlib.util.find_spec('usb') and importlib.util.find_spec('serial'),
                         'Huawei host dependencies not installed')
    def test_macos_uses_usb_and_keeps_xloader_patch_before_unsigned_uploads(self):
        blobs = {'xloader.img': b'xloader', 'teeos-decrypted.bin': b'tee',
                 'stock-lk-el2-tee.bin': b'lk'}
        with mock.patch.object(flash.platform, 'system', return_value='Darwin'), \
             mock.patch.object(flash, 'BootromUSB') as transport, \
             mock.patch.object(flash, 'wait_bootrom_tty') as tty, \
             mock.patch.object(flash, 'upload') as upload, \
             mock.patch('usb.core.find', return_value=object()):
            flash.enter_lk(blobs, {'tee_upload_address': 0x11000000})
        tty.assert_not_called()
        self.assertEqual([c.args[3] for c in upload.call_args_list],
                         [0x20000, 0x211ac, 0x11000000, 0x10000000])
        self.assertEqual(upload.call_args_list[1].args[2], bytes.fromhex('00207047'))
        self.assertTrue(all(c.args[0] is transport.return_value.__enter__.return_value
                            for c in upload.call_args_list))

    def test_readback_mismatch_fails(self):
        image = b'new kernel' * 64
        report = {}
        with tempfile.TemporaryDirectory() as work:
            with self.assertRaisesRegex(RuntimeError, 'readback mismatch'):
                flash.install(FakeFactory(image, corrupt=True), image, Path(work), report, lambda: None)
        self.assertFalse(report['readback_verified'])

    def test_wrong_partition_size_stops_before_flash(self):
        image = b'new kernel' * 64
        fb = FakeFactory(image, wrong_size=True)
        with tempfile.TemporaryDirectory() as work:
            with self.assertRaisesRegex(RuntimeError, 'partition size'):
                flash.install(fb, image, Path(work), {}, lambda: None)
        self.assertFalse(fb.flashed)
        self.assertFalse(fb.reads)

    def test_verification_uses_active_lock_state_instead_of_nv_fallback(self):
        import struct
        manifest = dict(patch_offset=0x10b634, patched_instruction='52807921',
                        tee_patch_offset=0x10bd3c, tee_patch_instruction='1406c0b1',
                        diagnostic_dispatch_patch=0x21550, tee_staging=0x01000000,
                        tee_bytes=4)
        memory = {0x10b634: struct.pack('<I', 0x52807921),
                  0x10bd3c: struct.pack('<I', 0x1406c0b1),
                  0x21550: struct.pack('<I', 0xd503201f), 0x01000000: b'tee!'}
        fb = SimpleNamespace(read=mock.Mock(side_effect=lambda kind, address, size: memory[address]),
                             command=mock.Mock(return_value=b' FB LockState: LOCKED\n USER LockState: UNLOCKED\n'))
        with mock.patch.object(flash, 'sha', return_value=flash.TEE_SHA):
            self.assertEqual(flash.verify_lk(fb, manifest),
                             dict(fblock='LOCKED', userlock='UNLOCKED'))
            fb.command.assert_called_once_with('oem lock-state info')
            fb.command.return_value = b' FB LockState: UNLOCKED\n USER LockState: LOCKED\n'
            with self.assertRaisesRegex(RuntimeError, 'USER lock'):
                flash.verify_lk(fb, manifest)
