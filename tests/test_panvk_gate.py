"""Exercise the hardware verdict without opening USB or submitting GPU work."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[1] / 'scripts/anne/panvk-gate.py'
spec = importlib.util.spec_from_file_location('panvk_gate', SCRIPT)
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


class PanvkGate(unittest.TestCase):
    def run_verdict(self, after='', gles_extra='', after_gles=''):
        responses = [
            '', '',
            'GPU: Vulkan status=0 frames=38 checksum=1234 display_restored=1\n',
            'panvk: T830 LOAD preservation verified\n' + after,
            'GPU: Mali-T830 frames=295 display restored\n' + gles_extra,
            after_gles,
        ]
        link = mock.Mock()
        with tempfile.TemporaryDirectory() as work, \
             mock.patch.object(gate, 'open_console', return_value=link), \
             mock.patch.object(gate.Session, 'run', side_effect=responses) as run:
            result = gate.run_gate(Path(work), 1, require_load=True)
            self.assertEqual(json.loads((Path(work) / 'phone-test.json').read_text()), result)
            self.assertEqual(run.call_args_list[-2].args[0], 'diag gpu')
            self.assertEqual(run.call_args_list[-1].args[0], 'diag log')
        link.close.assert_called_once()
        return result

    def test_clean_execution_passes(self):
        result = self.run_verdict()
        self.assertTrue(result['passed'])
        self.assertEqual(result['fault_markers'], [])

    def test_page_fault_fails_despite_successful_pixels_and_gles(self):
        for marker in ('Unhandled Page fault', 'JOB_BUS_FAULT', 'TRANSLATION_FAULT'):
            with self.subTest(marker=marker):
                result = self.run_verdict(after=marker)
                self.assertTrue(result['phone_gpu_execution'])
                self.assertTrue(result['gles_after_vulkan'])
                self.assertFalse(result['passed'])
                self.assertIn(marker, result['fault_markers'])

    def test_fault_in_gles_output_fails(self):
        result = self.run_verdict(gles_extra='Unhandled Page fault in AS0\n')
        self.assertFalse(result['passed'])
        self.assertIn('Unhandled Page fault', result['fault_markers'])

    def test_fault_retained_only_after_gles_fails(self):
        result = self.run_verdict(after_gles='js fault, status=JOB_BUS_FAULT\n')
        self.assertFalse(result['passed'])
        self.assertIn('JOB_BUS_FAULT', result['fault_markers'])

    def test_enumeration_failure_still_writes_failed_report(self):
        with tempfile.TemporaryDirectory() as work, \
             mock.patch.object(gate, 'open_console', side_effect=RuntimeError('no CDC console')):
            result = gate.run_gate(Path(work), 1)
            self.assertFalse(result['passed'])
            self.assertEqual(result['error'], 'no CDC console')
            self.assertEqual(json.loads((Path(work) / 'phone-test.json').read_text()), result)


if __name__ == '__main__':
    unittest.main()
