"""Build/BootROM workflow contracts, with all USB and writes substituted."""
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('huawei_workflow', ROOT / 'scripts/anne/cli.py')
cli = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(cli)


def package(directory):
    directory = directory.resolve()
    directory.mkdir()
    image = b'ANDROID!' + bytes(0x1800000 - 8)
    digest = hashlib.sha256(image).hexdigest()
    (directory / 'ame-native-kernel.img').write_bytes(image)
    (directory / 'manifest.json').write_text(json.dumps(dict(
        board='huawei-anne', model='ANE-LX1', partition='kernel',
        partition_bytes=len(image), native_image_sha256=digest,
        avb={'local_signature_and_hash_verified': True})))
    return directory


class HuaweiWorkflow(unittest.TestCase):
    def test_low_disk_space_stops_gpu_build_before_starting_tools(self):
        args = cli.parser().parse_args(['build'])
        for system in ('Darwin', 'Linux'):
            with self.subTest(system=system), \
                 patch.object(cli.platform, 'system', return_value=system), \
                 patch.object(cli.shutil, 'disk_usage', return_value=SimpleNamespace(free=1024**3)), \
                 patch.object(cli, 'execute') as execute, \
                 patch.object(cli.subprocess, 'run') as run:
                with self.assertRaisesRegex(ValueError, '12 GiB'):
                    cli.build_guest(args)
                execute.assert_not_called()
                run.assert_not_called()

    def test_public_help_from_another_directory(self):
        with tempfile.TemporaryDirectory() as work:
            result = subprocess.run([str(ROOT / 'scripts/huawei'), '--help'], cwd=work,
                                    capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('{build,run,build-run,console}', result.stdout)
        self.assertNotIn('_guest', result.stdout)

    def test_native_build_configures_clean_tree_and_builds_apps_before_packaging(self):
        args = cli.parser().parse_args(['build', '--native-only', '--dry-run'])
        with patch.object(cli, 'execute') as execute, patch.object(cli, 'remember') as remember:
            output = cli.build(args)
        calls = [call.args[0] for call in execute.call_args_list]
        self.assertIn('-DANKO_BOARD=huawei-anne', calls[0])
        self.assertIn('-DMOCHI_SYSTEM_PROFILE=huawei-anne-usb', calls[0])
        self.assertIn('-DDANGO_LINUX_ARTIFACT_DIR=', calls[0])
        self.assertIn('-DPython3_EXECUTABLE=' + sys.executable, calls[0])
        self.assertEqual(calls[1][-len(cli.TARGETS):], list(cli.TARGETS))
        self.assertIn(cli.TOOLS / 'build-native-package.py', calls[2])
        self.assertIn(output, calls[2])
        remember.assert_not_called()

    def test_guest_builds_both_mesa_drivers_then_kernel_renderer_and_manifest(self):
        with tempfile.TemporaryDirectory() as work:
            source = Path(work)
            with patch.object(cli, 'execute') as execute:
                cli.guest_worker(source, source / 'guest', 2)
        scripts = [call.args[0][0].name for call in execute.call_args_list]
        self.assertEqual(scripts, ['build-panfrost-mesa.sh', 'build-panvk-mesa.sh',
                                   'build-panfrost-linux.sh'])
        env = execute.call_args_list[-1].kwargs['env']
        self.assertEqual(env['DANGO_LINUX_TIMER_HZ'], '1920000')
        self.assertEqual(env['DANGO_PANFROST_RENDERER'], 'gles')
        self.assertEqual(env['DANGO_PANFROST_HARDWARE'], '1')
        self.assertTrue(env['DANGO_PANVK_PREFIX'].endswith('mesa-panvk/install'))

    def test_gpu_build_plans_guest_before_configure_and_embeds_same_directory(self):
        args = cli.parser().parse_args(['build', '--dry-run'])
        with patch.object(cli.platform, 'system', return_value='Darwin'), \
             patch.object(cli, 'execute') as execute, patch.object(cli.subprocess, 'run') as run:
            cli.build(args)
        calls = [call.args[0] for call in execute.call_args_list]
        self.assertEqual(calls[0][:2], ['docker', 'build'])
        self.assertEqual(calls[1][:2], ['docker', 'run'])
        configure = next(c for c in calls if '-DMOCHI_SYSTEM_PROFILE=huawei-anne-gpu' in c)
        self.assertIn('-DDANGO_LINUX_ARTIFACT_DIR=' + str(cli.WORK / 'guest'), configure)
        worker = next(c for c in calls if '_guest' in c)
        self.assertIn('--export', worker)
        self.assertTrue(any('type=volume' in str(part) for part in worker))
        run.assert_not_called()  # a plan cannot contact even the Docker daemon

    def test_export_copies_only_runtime_files_and_rejects_incomplete_guest(self):
        with tempfile.TemporaryDirectory() as work:
            base = Path(work)
            source, target = base / 'source', base / 'export'
            source.mkdir()
            (source / 'Image').write_bytes(b'kernel')
            with self.assertRaisesRegex(ValueError, 'missing'):
                cli.export_guest(source, target)
            self.assertFalse(target.exists())
            for name in ('initramfs.cpio', 'dango-linux.dtb', 'dango-agent', 'manifest.toml'):
                (source / name).write_text(name)
            (source / 'kernel-build').mkdir()
            cli.export_guest(source, target)
            self.assertEqual((target / 'Image').read_bytes(), b'kernel')
            self.assertEqual((target / 'manifest.toml').read_text(), 'manifest.toml')
            self.assertFalse((target / 'kernel-build').exists())

    def test_bad_prebuilt_guest_stops_before_configure(self):
        with tempfile.TemporaryDirectory() as work:
            directory = Path(work)
            (directory / 'manifest.toml').write_text('profile="hello"\n')
            args = cli.parser().parse_args(['build', '--guest-artifacts', str(directory)])
            with patch.object(cli, 'execute') as execute:
                with self.assertRaisesRegex(ValueError, 'Prebuilt guest'):
                    cli.build_guest(args)
            execute.assert_not_called()

    def test_recorded_package_is_exact_and_rejects_changes(self):
        with tempfile.TemporaryDirectory() as work:
            base = Path(work)
            selected = package(base / 'selected')
            with patch.object(cli, 'LATEST', base / 'latest.json'):
                cli.remember(selected, base / 'build', 'huawei-anne-usb')
                # Neither a newer package nor its directory name changes run's selection.
                package(base / 'zzzz-newer')
                self.assertEqual(cli.selected_package(None), selected)
                with (selected / 'ame-native-kernel.img').open('r+b') as image:
                    image.seek(10)
                    image.write(b'changed')
                with self.assertRaisesRegex(ValueError, 'hash mismatch'):
                    cli.selected_package(None)

    def test_failed_package_does_not_replace_latest_record(self):
        with tempfile.TemporaryDirectory() as work:
            base = Path(work)
            latest = base / 'latest.json'
            latest.write_text('previous-good-record')
            with patch.object(cli, 'LATEST', latest):
                with self.assertRaises(FileNotFoundError):
                    cli.remember(base / 'missing', base, 'huawei-anne-gpu')
            self.assertEqual(latest.read_text(), 'previous-good-record')

    def test_missing_record_never_guesses_an_old_package(self):
        with tempfile.TemporaryDirectory() as work, \
             patch.object(cli, 'LATEST', Path(work) / 'missing.json'):
            with self.assertRaisesRegex(ValueError, 'build first'):
                cli.selected_package(None)

    def test_run_has_no_build_and_pins_image_through_bootrom_to_install(self):
        with tempfile.TemporaryDirectory() as work:
            selected = package(Path(work) / 'package')
            args = cli.parser().parse_args(['run', '--package', str(selected), '--backup'])
            flash, gate = Mock(), Mock()
            flash.inputs.return_value = (b'captured image', {'firmware': 'pinned'}, {'lk': b'data'})
            gate.wait_for_loader.return_value = 'HANDSET'
            with patch.object(cli, 'tool', side_effect=[flash, gate]), \
                 patch.object(cli, 'execute') as execute, \
                 patch.dict(sys.modules, {'serial': Mock(), 'usb': Mock(), 'usb.core': Mock()}):
                cli.run(args)
            gate.wait_for_loader.assert_called_once_with(flash, {'lk': b'data'},
                                                        {'firmware': 'pinned'}, 0)
            command = execute.call_args.args[0]
            self.assertNotIn('cmake', command)
            self.assertIn('--running', command)
            self.assertIn('--backup', command)
            self.assertIn(hashlib.sha256(b'captured image').hexdigest(), command)
            self.assertEqual(command[command.index('--serial') + 1], 'HANDSET')

    def test_dry_run_verifies_local_inputs_without_usb_or_subprocess(self):
        with tempfile.TemporaryDirectory() as work:
            selected = package(Path(work) / 'package')
            args = cli.parser().parse_args(['run', '--package', str(selected), '--dry-run'])
            flash = Mock()
            flash.inputs.return_value = (b'image', {}, {})
            with patch.object(cli, 'tool', return_value=flash) as tool, \
                 patch.object(cli, 'execute') as execute:
                cli.run(args)
            tool.assert_called_once_with('flash-el2')
            flash.inputs.assert_called_once_with(selected, args.firmware)
            execute.assert_not_called()

    def test_failed_local_validation_never_discovers_or_uploads(self):
        with tempfile.TemporaryDirectory() as work:
            selected = package(Path(work) / 'package')
            args = cli.parser().parse_args(['run', '--package', str(selected)])
            flash = Mock()
            flash.inputs.side_effect = ValueError('firmware hash mismatch')
            with patch.object(cli, 'tool', return_value=flash) as tool, \
                 patch.object(cli, 'execute') as execute:
                with self.assertRaisesRegex(ValueError, 'firmware hash'):
                    cli.run(args)
            tool.assert_called_once_with('flash-el2')
            execute.assert_not_called()

    def test_failed_build_run_cannot_flash(self):
        with patch.object(cli, 'build', side_effect=ValueError('build failed')), \
             patch.object(cli, 'run') as run:
            self.assertEqual(cli.main(['build-run']), 1)
        run.assert_not_called()

    def test_ambiguous_bootrom_devices_are_rejected_before_upload(self):
        gate = cli.tool('panvk-gate')
        flash = Mock()
        with patch.object(gate, 'find', return_value=[Mock(), Mock()]):
            with self.assertRaisesRegex(RuntimeError, 'More than one'):
                gate.wait_for_loader(flash, {}, {}, 0)
        flash.enter_lk.assert_not_called()

    def test_existing_ram_loader_is_reused_without_bootrom_upload(self):
        gate = cli.tool('panvk-gate')
        flash = Mock()
        with patch.object(gate, 'find', side_effect=[[SimpleNamespace(serial_number='ONE')], []]):
            self.assertEqual(gate.wait_for_loader(flash, {}, {}, 1), 'ONE')
        flash.enter_lk.assert_not_called()

    def test_one_bootrom_and_one_ram_loader_are_also_ambiguous(self):
        gate = cli.tool('panvk-gate')
        flash = Mock()
        with patch.object(gate, 'find', return_value=[Mock()]):
            with self.assertRaisesRegex(RuntimeError, 'More than one'):
                gate.wait_for_loader(flash, {}, {}, 0)
        flash.enter_lk.assert_not_called()


if __name__ == '__main__':
    unittest.main()
