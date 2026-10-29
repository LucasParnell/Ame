import importlib.util
from pathlib import Path
import struct
import subprocess
import unittest
from unittest import mock

SCRIPT = Path(__file__).resolve().parents[1] / 'scripts/anne/usb-ep0-trial.py'
spec = importlib.util.spec_from_file_location('usb_ep0_trial', SCRIPT)
trial = importlib.util.module_from_spec(spec)
spec.loader.exec_module(trial)


def reply(mode):
    return struct.pack('<QQ8I', 1, 1, 0, 0, 0, 0, mode, 0, 0, 0)


def class_reply(request_id=1):
    return struct.pack('<12I', 0x31534341, 0x72021, 0, 7, 0, 0, request_id, 0,
                       0x1c200, 0x80000, 0, 0)


class UsbEp0Trial(unittest.TestCase):
    def run_trial(self, device, capture):
        with mock.patch.object(trial.usb.util, 'claim_interface') as claim, \
             mock.patch.object(trial.usb.util, 'release_interface') as release, \
             mock.patch.object(trial.usb.util, 'dispose_resources'), \
             mock.patch.object(trial, 'capture', capture):
            report = trial.run_trial(device, 2, Path('/unused'))
        claim.assert_called_once_with(device, 0)
        release.assert_called_once_with(device, 0)
        return report

    def test_timeout_captured_before_cancel_and_driver_restored(self):
        device = mock.Mock()
        device.ctrl_transfer.side_effect = [class_reply(), reply(2), trial.usb.core.USBError('timeout'),
                                            class_reply(2), bytes.fromhex('00c20100000008'), reply(0)]
        capture = mock.Mock(side_effect=lambda _: device.ctrl_transfer.call_count == 3)
        report = self.run_trial(device, capture)
        self.assertFalse(report['set_line_completed'])
        self.assertTrue(report['snapshot_captured'])
        self.assertTrue(report['cancelled'])
        self.assertTrue(report['driver_reattached'])
        device.attach_kernel_driver.assert_called_once_with(0)
        self.assertTrue(report['class_state_matches_trial'])
        self.assertTrue(report['class_data_matches_sent'])
        self.assertEqual(device.ctrl_transfer.call_args_list[2].args[:2], (0x21, 0x20))

    def test_unsupported_arm_stops_before_class_data(self):
        device = mock.Mock()
        device.ctrl_transfer.side_effect = [class_reply(), reply(0), reply(0)]
        capture = mock.Mock()
        report = self.run_trial(device, capture)
        self.assertIn('did not acknowledge', report['error'])
        capture.assert_not_called()
        self.assertTrue(all(c.args[1] == 0x5d for c in device.ctrl_transfer.call_args_list[1:]))
        device.attach_kernel_driver.assert_called_once_with(0)

    def test_capture_failure_restores_only_previously_attached_driver(self):
        device = mock.Mock()
        device.is_kernel_driver_active.return_value = False
        device.ctrl_transfer.side_effect = [class_reply(), reply(2), 7, reply(0)]
        report = self.run_trial(device, mock.Mock(side_effect=subprocess.TimeoutExpired('snapshot', 20)))
        self.assertTrue(report['set_line_completed'])
        self.assertIn('error', report)
        device.detach_kernel_driver.assert_not_called()
        device.attach_kernel_driver.assert_not_called()

    def test_old_retained_payload_is_not_attributed_to_trial(self):
        device = mock.Mock()
        device.ctrl_transfer.side_effect = [class_reply(17), reply(2),
                                            trial.usb.core.USBError('pipe'), class_reply(17),
                                            bytes.fromhex('00c20100000008'), reply(0)]
        report = self.run_trial(device, mock.Mock(return_value=True))
        self.assertFalse(report['class_state_matches_trial'])
        self.assertNotIn('class_data_matches_sent', report)
        self.assertEqual(report['class_state_before']['request_id'], 17)
        self.assertTrue(report['driver_reattached'])


if __name__ == '__main__':
    unittest.main()
