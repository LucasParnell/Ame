#!/usr/bin/env python3
"""Arm one bounded EP0 OUT trial after enumeration, then send CDC line coding.

Requires firmware with vendor request 0x5d. Mode 0 preserves the baseline;
mode 1 tries CNAK alone; mode 2 tries a full receive window followed by CNAK.
The CDC host driver is temporarily detached and restored. No flash, arbitrary
register write, bus reset, or serial-port open is performed.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import sys
import time

import usb.core
import usb.util

ROOT = Path(__file__).resolve().parents[2]


def arm(device, mode):
    raw = bytes(device.ctrl_transfer(0xc0, 0x5d, mode, 0, 48, timeout=2000))
    values = struct.unpack('<QQ8I', raw)
    if values[6] != mode:
        raise RuntimeError('firmware did not acknowledge the requested trial mode')
    return {'mode': values[6], 'request_id': values[0], 'generation': values[1],
            'out_control': values[2], 'out_size': values[4]}


def capture(output):
    result = subprocess.run([sys.executable, str(ROOT / 'scripts/anne/usb-state.py'),
                             '--output', str(output / 'after-set-line.json')],
                            capture_output=True, text=True, timeout=20)
    (output / 'snapshot.log').write_text(result.stdout + result.stderr)
    return result.returncode == 0


def class_state(device):
    values = struct.unpack('<12I', bytes(device.ctrl_transfer(0xc0, 0x5e, 0, 0, 48,
                                                             timeout=2000)))
    if values[0] != 0x31534341:
        raise RuntimeError('unsupported retained class snapshot')
    return {'setup_word': values[1], 'index_value': values[2], 'received_bytes': values[3],
            'reply_status': struct.unpack('<i', struct.pack('<I', values[4]))[0],
            'reply_bytes': values[5], 'request_id': values[6] | (values[7] << 32),
            'data': struct.pack('<4I', *values[8:]).hex()}


def run_trial(device, mode, output, baud=115200, prime_controls=0):
    coding = struct.pack('<IBBB', baud, 0, 0, 8)
    report = {'mode': mode, 'baud': baud, 'prime_controls': prime_controls,
              'coding_sent': coding.hex(),
              'set_line_completed': False, 'persistent_writes': False}
    detached = claimed = False
    try:
        if device.is_kernel_driver_active(0):
            device.detach_kernel_driver(0)
            detached = True
        usb.util.claim_interface(device, 0)
        claimed = True
        # A retained class snapshot can survive reset/replug and predate this
        # trial. Preserve it before arming so old bytes cannot look like a new
        # reception. v34 lacks this optional v35 diagnostic.
        try:
            report['class_state_before'] = class_state(device)
        except Exception as error:
            report['class_state_before_error'] = str(error)
        report['armed'] = arm(device, mode)
        for _ in range(prime_controls):
            device.ctrl_transfer(0x21, 0x22, 3, 0, b'', timeout=2000)
        started = time.monotonic()
        try:
            written = device.ctrl_transfer(0x21, 0x20, 0, 0, coding, timeout=2000)
            report['bytes_written'] = written
            report['set_line_completed'] = written == 7
        except usb.core.USBError as error:
            report['set_line_error'] = str(error)
        report['set_line_elapsed_ms'] = round((time.monotonic() - started) * 1000)
        # Freeze/capture the tested request before any new class request or
        # host driver reattachment can replace its retained history.
        report['snapshot_captured'] = capture(output)
        try:
            report['class_state'] = class_state(device)
            # With the CDC driver detached and this tool as sole EP0 owner,
            # only the optional primers separate arm from SET_LINE_CODING.
            # Extra host traffic makes this comparison inconclusive (false),
            # rather than assigning an unrelated retained request to the trial.
            report['expected_request_id'] = report['armed']['request_id'] + prime_controls + 1
            after = report['class_state']
            before = report.get('class_state_before')
            report['class_state_matches_trial'] = bool(
                after['request_id'] == report['expected_request_id'] and
                after['setup_word'] == 0x00072021 and after['index_value'] == 0 and
                (before is None or after['request_id'] != before['request_id']))
            if report['class_state_matches_trial']:
                report['class_data_matches_sent'] = bool(
                    after['received_bytes'] == len(coding) and
                    bytes.fromhex(after['data'])[:len(coding)] == coding)
        except Exception as error:
            report['class_state_error'] = str(error)
        try:
            report['coding_after'] = bytes(device.ctrl_transfer(0xa1, 0x21, 0, 0, 7,
                                                               timeout=2000)).hex()
        except usb.core.USBError as error:
            report['get_line_error'] = str(error)
    except Exception as error:
        report['error'] = str(error)
    finally:
        if claimed:
            try:
                report['cancelled'] = arm(device, 0)['mode'] == 0
            except Exception as error:
                report['cancel_error'] = str(error)
            try:
                usb.util.release_interface(device, 0)
            except usb.core.USBError as error:
                report['release_error'] = str(error)
        if detached:
            try:
                device.attach_kernel_driver(0)
                report['driver_reattached'] = True
            except usb.core.USBError as error:
                report['driver_reattached'] = False
                report['reattach_error'] = str(error)
        usb.util.dispose_resources(device)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', required=True, type=int, choices=(0, 1, 2))
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--baud', type=int, default=115200)
    parser.add_argument('--prime-controls', type=int, choices=(0, 1, 2), default=0,
                        help='send this many no-data CDC control requests before DATA OUT')
    args = parser.parse_args()
    if not 1 <= args.baud <= 4000000:
        parser.error('--baud must be between 1 and 4000000')
    if (args.output / 'report.json').exists():
        parser.error('use a new output directory to preserve the previous trial')
    devices = list(usb.core.find(find_all=True, idVendor=0x1209, idProduct=0x0001))
    if len(devices) != 1:
        parser.error('expected exactly one Ame USB device')
    device = devices[0]
    config = device.get_active_configuration()
    if config.bConfigurationValue != 1 or config[(0, 0)].bInterfaceClass != 2:
        parser.error('Ame CDC must have enumerated before a trial can be armed')
    args.output.mkdir(parents=True, exist_ok=True)
    report = run_trial(device, args.mode, args.output, args.baud, args.prime_controls)
    (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    return 0 if report['set_line_completed'] else 1


if __name__ == '__main__':
    sys.exit(main())
