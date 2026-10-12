import contextlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch, Mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('boot_runner', ROOT / 'scripts/test/boot.py')
boot_runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boot_runner)


class BootRunner(unittest.TestCase):
    def test_reused_logs_cannot_pass_a_failed_boot(self):
        with tempfile.TemporaryDirectory(prefix='ame-boot-test-') as work:
            for name in ('host.log', 'guest.log'):
                Path(work, name).write_text('all services ready\n')
            process = Mock(returncode=1)
            process.poll.return_value = 1
            with patch.object(boot_runner.subprocess, 'Popen', return_value=process), \
                    contextlib.redirect_stdout(io.StringIO()):
                self.assertFalse(boot_runner.run(work, 'direct', 1, 1,
                                 markers=['all services ready'], output=work))
            self.assertIn('qemu exited 1', Path(work, 'result.json').read_text())

    def test_el1_bringup_disables_virtualization_and_records_the_requested_cpu(self):
        with tempfile.TemporaryDirectory(prefix='ame-boot-el1-') as work:
            process = Mock(returncode=1)
            process.poll.return_value = 1
            with patch.object(boot_runner.subprocess, 'Popen', return_value=process) as launch, \
                    contextlib.redirect_stdout(io.StringIO()):
                self.assertFalse(boot_runner.run(work, 'direct', 1, 1, output=work,
                                                entry_el=1, cpu='cortex-a53'))
            result = json.loads(Path(work, 'result.json').read_text())
            command = launch.call_args.args[0]
            self.assertEqual(result['command'], command)
            self.assertEqual(command[command.index('-machine') + 1],
                             'virt,virtualization=off,gic-version=2')
            self.assertEqual(command[command.index('-cpu') + 1], 'cortex-a53')
            self.assertEqual(result['requested_entry_el'], 1)

    def test_invalid_entry_level_is_rejected_before_launch(self):
        with patch.object(boot_runner.subprocess, 'Popen') as launch:
            with self.assertRaisesRegex(ValueError, 'entry_el'):
                boot_runner.run('.', 'direct', 1, 1, entry_el=3)
            launch.assert_not_called()
