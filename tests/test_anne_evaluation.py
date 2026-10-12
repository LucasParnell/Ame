import hashlib
import importlib.util
import io
import json
import subprocess
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('anne_evaluation',
                                            ROOT / 'scripts/anne/build-recovery-evaluation.py')
evaluation = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(evaluation)
RUN_SPEC = importlib.util.spec_from_file_location('anne_evaluation_runner',
                                                ROOT / 'scripts/anne/run-recovery-evaluation.py')
runner = importlib.util.module_from_spec(RUN_SPEC)
RUN_SPEC.loader.exec_module(runner)


class AnneEvaluation(unittest.TestCase):
    def archive(self, destination, entries):
        with tarfile.open(destination, 'w:gz') as archive:
            for name, target in entries:
                member = tarfile.TarInfo(name)
                if target is not None:
                    member.type = tarfile.SYMTYPE
                    member.linkname = target
                    archive.addfile(member)
                else:
                    member.size = 4
                    archive.addfile(member, io.BytesIO(b'data'))

    def test_absolute_rootfs_links_become_internal_relative_links(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'source.tar.gz'
            output = Path(directory) / 'output.tar.gz'
            self.archive(source, [('bin/busybox', None), ('usr/bin/test', '/bin/busybox')])
            evaluation.normalize_rootfs(source, output)
            with tarfile.open(output) as archive:
                self.assertEqual(archive.getmember('usr/bin/test').linkname, '../../bin/busybox')
                self.assertEqual(archive.extractfile('bin/busybox').read(), b'data')

    def test_rejects_archive_path_and_link_escape(self):
        with tempfile.TemporaryDirectory() as directory:
            for entry in [('../outside', None), ('/outside', None), ('bin/link', '../../outside')]:
                source = Path(directory) / 'source.tar.gz'
                self.archive(source, [entry])
                with self.assertRaises(ValueError):
                    evaluation.normalize_rootfs(source, Path(directory) / 'output.tar.gz')

    def test_rejects_writes_through_archive_link_parent(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'source.tar.gz'
            self.archive(source, [('lib', 'usr/lib'), ('lib/file', None)])
            with self.assertRaises(ValueError):
                evaluation.normalize_rootfs(source, Path(directory) / 'output.tar.gz')

    def test_hash_mismatch_is_rejected_without_download(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'package.apk'
            path.write_bytes(b'changed')
            with self.assertRaises(ValueError):
                evaluation.verified_file(path, hashlib.sha256(b'original').hexdigest())

    def test_existing_output_is_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            marker = path / 'keep'
            marker.write_text('existing')
            with self.assertRaises(FileExistsError):
                evaluation.build(path / 'missing-cache', path / 'missing-build', path)
            self.assertEqual(marker.read_text(), 'existing')

    def test_runtime_lock_is_complete_and_launch_is_headless(self):
        lock = json.loads(evaluation.LOCK.read_text())
        self.assertEqual(lock['architecture'], 'aarch64')
        packages = {record['name']: record for record in lock['packages']}
        self.assertEqual(len(packages), len(lock['packages']))
        self.assertIn('qemu-system-aarch64', packages)
        self.assertIn('musl', packages)
        for record in packages.values():
            self.assertRegex(record['sha256'], r'^[0-9a-f]{64}$')
            self.assertGreater(record['size'], 0)
        self.assertIn('--no-scripts', evaluation.SETUP)
        self.assertIn('--repositories-file /dev/null --no-network', evaluation.SETUP)
        self.assertNotIn('--allow-untrusted', evaluation.SETUP)
        self.assertIn('-accel tcg,thread=single', evaluation.LAUNCH)
        self.assertIn('-display none -nic none', evaluation.LAUNCH)
        self.assertNotIn('/dev/block', evaluation.LAUNCH)

    def test_unknown_phone_is_rejected_before_any_push_or_execution(self):
        calls = []

        def run(command, **kwargs):
            calls.append(command)
            responses = {'getprop ro.product.device': 'other-phone',
                         'getprop ro.twrp.version': '3.7.0', 'id': 'uid=0(root)'}
            text = ('List of devices attached\nSYNTHETIC\trecovery\n' if command[1:] == ['devices']
                    else responses[command[-1]])
            return subprocess.CompletedProcess(command, 0, text, '')

        with self.assertRaises(ValueError):
            runner.Device('/fake/adb', run=run)
        self.assertTrue(all('push' not in call for call in calls))
        self.assertTrue(all('setup.sh' not in ' '.join(call) for call in calls))

    def test_ambiguous_connection_is_rejected_before_phone_commands(self):
        calls = []

        def run(command, **kwargs):
            calls.append(command)
            return subprocess.CompletedProcess(command, 0, 'A\trecovery\nB\tdevice\n', '')

        with self.assertRaises(ValueError):
            runner.Device('/fake/adb', run=run)
        self.assertEqual(calls, [['/fake/adb', 'devices']])

    def test_process_must_match_full_emulator_argument(self):
        device = object.__new__(runner.Device)
        prefix = '/tmp/ame-evaluation-test'
        device.shell = lambda *args, **kwargs: subprocess.CompletedProcess([], 0,
            prefix + '/rootfs/usr/bin/qemu-system-aarch64-unrelated\0', '')
        self.assertFalse(device.process_matches('123', prefix))
        device.shell = lambda *args, **kwargs: subprocess.CompletedProcess([], 0,
            'loader\0' + prefix + '/rootfs/usr/bin/qemu-system-aarch64\0-machine\0', '')
        self.assertTrue(device.process_matches('123', prefix))
        self.assertFalse(device.process_matches('123; command', prefix))

    def test_staging_package_rejects_symlink_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'external').symlink_to('/tmp', target_is_directory=True)
            with self.assertRaisesRegex(ValueError, 'symlinks'):
                runner.verify_package(root)

    def test_transport_loss_preserves_failure_report_when_cleanup_also_fails(self):
        class LostDevice:
            serial, twrp = 'SYNTHETIC-PRIVATE', '3.7.0'

            def call(self, *args, **kwargs):
                return subprocess.CompletedProcess([], 0, '', '')

            def shell(self, arguments, **kwargs):
                if arguments[0] == 'test':
                    return subprocess.CompletedProcess([], 1, '', '')
                raise RuntimeError('disconnected SYNTHETIC-PRIVATE')

        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'evidence'
            with mock.patch.object(runner, 'verify_package'), \
                    mock.patch.object(runner, 'Device', return_value=LostDevice()):
                with self.assertRaisesRegex(RuntimeError, 'disconnected'):
                    runner.evaluate(Path(directory), output, '/fake/adb')
            report = json.loads((output / 'result.json').read_text())
            self.assertFalse(report['services_ready'])
            self.assertFalse(report['emulator_stopped'])
            self.assertIn('cleanup_error', report)
            self.assertNotIn('SYNTHETIC-PRIVATE', (output / 'result.json').read_text())


if __name__ == '__main__':
    unittest.main()
