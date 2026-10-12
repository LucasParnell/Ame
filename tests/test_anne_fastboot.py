import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('anne_fastboot', ROOT / 'scripts/anne/inspect-fastboot.py')
fastboot = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(fastboot)


class FakeFastboot:
    def __init__(self):
        self.calls = []
        self.targets = b'SYNTHETIC-SERIAL-ONLY\tfastboot\n'
        self.timeout = None

    def __call__(self, command, **kwargs):
        self.calls.append(command)
        if command[1:] == ['devices']:
            return subprocess.CompletedProcess(command, 0, self.targets, b'')
        arguments = tuple(command[3:])
        if arguments == self.timeout:
            raise subprocess.TimeoutExpired(command, 1)
        responses = {
            ('oem', 'get-bootinfo'): b'(bootloader) unlocked\nOKAY\n',
            ('oem', 'get-product-model'): b'(bootloader) ANE-LX1\nOKAY\n',
            ('oem', 'get-build-number'): b'(bootloader) ANE-LX1 synthetic-build\nOKAY\n',
            ('getvar', 'max-download-size'): b'max-download-size: 0x10000000\n',
        }
        return subprocess.CompletedProcess(command, 0, b'', responses[arguments])


class AnneFastboot(unittest.TestCase):
    def test_remote_failure_with_successful_process_exit_is_not_success(self):
        response = subprocess.CompletedProcess([], 0, b'',
                    b"getvar:product FAILED (remote: 'Command not allowed')\n")
        self.assertEqual(fastboot.query_result(response, 'private')['status'], 'unsupported-or-failed')

    def test_reports_model_and_unlock_state_but_does_not_infer_recovery(self):
        with tempfile.TemporaryDirectory() as work:
            runner = FakeFastboot()
            output, report = fastboot.inspect('/fake/fastboot', Path(work) / 'capture', run=runner)
            summary = report['summary']
            self.assertEqual(summary['model'], 'ANE-LX1')
            self.assertTrue(summary['bootloader_unlocked'])
            self.assertIsNone(summary['twrp_installed'])
            self.assertFalse(summary['native_boot_ready'])
            self.assertEqual(json.loads((output / 'report.json').read_text()), report)
            self.assertNotIn('SYNTHETIC-SERIAL-ONLY', (output / 'report.json').read_text())
            self.assertEqual(runner.calls[0], ['/fake/fastboot', 'devices'])
            self.assertEqual(runner.calls[1:], [['/fake/fastboot', '-s', 'SYNTHETIC-SERIAL-ONLY', *args]
                                             for args in fastboot.QUERIES.values()])

    def test_timeout_stops_further_device_queries(self):
        with tempfile.TemporaryDirectory() as work:
            runner = FakeFastboot()
            runner.timeout = ('oem', 'get-bootinfo')
            _, report = fastboot.inspect('/fake/fastboot', Path(work) / 'capture', run=runner)
            self.assertEqual(len(runner.calls), 2)
            self.assertEqual(report['queries']['bootloader_status']['status'], 'timeout')
            self.assertIsNone(report['summary']['bootloader_unlocked'])

    def test_ambiguous_device_selection_is_rejected_before_queries(self):
        with tempfile.TemporaryDirectory() as work:
            runner = FakeFastboot()
            runner.targets += b'OTHER-TRANSPORT\tfastboot\n'
            output = Path(work) / 'capture'
            with self.assertRaises(ValueError):
                fastboot.inspect('/fake/fastboot', output, run=runner)
            self.assertFalse(output.exists())
            self.assertEqual(len(runner.calls), 1)
            _, report = fastboot.inspect('/fake/fastboot', output,
                                        serial='SYNTHETIC-SERIAL-ONLY', run=runner)
            self.assertTrue(report['summary']['target_confirmed'])

    def test_failed_status_and_wrong_model_do_not_become_verified_facts(self):
        report = {'queries': {
            'bootloader_status': {'status': 'unsupported-or-failed', 'response': 'unlocked FAILED'},
            'model': {'status': 'ok', 'response': '(bootloader) MAR-LX1'},
        }}
        summary = fastboot.summarize(report)
        self.assertFalse(summary['target_confirmed'])
        self.assertIsNone(summary['bootloader_unlocked'])

    def test_existing_output_is_not_overwritten(self):
        with tempfile.TemporaryDirectory() as work:
            output = Path(work)
            marker = output / 'preserved'
            marker.write_text('original')
            with self.assertRaises(FileExistsError):
                fastboot.inspect('/fake/fastboot', output, run=FakeFastboot())
            self.assertEqual(marker.read_text(), 'original')


if __name__ == '__main__':
    unittest.main()
