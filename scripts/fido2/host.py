#!/usr/bin/env python3
"""Small macOS CTAPHID transport probe using the system IOHID API.

Only opens Ame's FIDO usage collection. Does not grab keyboard/CDC interfaces.
"""
import argparse
import ctypes as c
import os
import hashlib
import json
from pathlib import Path
import time


class MacHid:
    def __init__(self):
        self.cf = c.CDLL('/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation')
        self.io = c.CDLL('/System/Library/Frameworks/IOKit.framework/IOKit')
        p = c.c_void_p
        self.callback_type = c.CFUNCTYPE(None, p, c.c_int, p, c.c_int, c.c_uint32, p, c.c_long)
        declarations = {
            'CFStringCreateWithCString': ([p, c.c_char_p, c.c_uint32], p),
            'CFNumberCreate': ([p, c.c_int, p], p),
            'CFDictionaryCreateMutable': ([p, c.c_long, p, p], p),
            'CFDictionarySetValue': ([p, p, p], None),
            'CFSetGetCount': ([p], c.c_long), 'CFSetGetValues': ([p, p], None),
            'CFRunLoopGetCurrent': ([], p),
            'CFRunLoopRunInMode': ([p, c.c_double, c.c_bool], c.c_int),
            'CFRelease': ([p], None),
        }
        for name, (args, result) in declarations.items():
            f = getattr(self.cf, name); f.argtypes = args; f.restype = result
        declarations = {
            'IOHIDManagerCreate': ([p, c.c_uint32], p),
            'IOHIDManagerSetDeviceMatching': ([p, p], None),
            'IOHIDManagerCopyDevices': ([p], p),
            'IOHIDManagerOpen': ([p, c.c_uint32], c.c_int),
            'IOHIDManagerClose': ([p, c.c_uint32], c.c_int),
            'IOHIDDeviceOpen': ([p, c.c_uint32], c.c_int),
            'IOHIDDeviceClose': ([p, c.c_uint32], c.c_int),
            'IOHIDDeviceScheduleWithRunLoop': ([p, p, p], None),
            'IOHIDDeviceUnscheduleFromRunLoop': ([p, p, p], None),
            'IOHIDDeviceRegisterInputReportCallback': ([p, p, c.c_long, self.callback_type, p], None),
            'IOHIDDeviceSetReport': ([p, c.c_int, c.c_long, p, c.c_long], c.c_int),
        }
        for name, (args, result) in declarations.items():
            f = getattr(self.io, name); f.argtypes = args; f.restype = result
        self.manager = self.io.IOHIDManagerCreate(None, 0)
        matching = self.cf.CFDictionaryCreateMutable(None, 0, None, None)
        self.match_values = []
        for key, value in [('VendorID', 0x1209), ('ProductID', 1),
                           ('PrimaryUsagePage', 0xf1d0), ('PrimaryUsage', 1)]:
            k = self.cf.CFStringCreateWithCString(None, key.encode(), 0x08000100)
            n = c.c_int32(value)
            v = self.cf.CFNumberCreate(None, 3, c.byref(n))
            self.match_values += [k, v]
            self.cf.CFDictionarySetValue(matching, k, v)
        self.io.IOHIDManagerSetDeviceMatching(self.manager, matching)
        self.cf.CFRelease(matching)
        status = self.io.IOHIDManagerOpen(self.manager, 0)
        if status: raise RuntimeError(f'IOHIDManagerOpen: {status:#x}')
        devices = self.io.IOHIDManagerCopyDevices(self.manager)
        if not devices or self.cf.CFSetGetCount(devices) != 1:
            raise RuntimeError('Expected exactly one Ame FIDO HID collection')
        array = (p * 1)()
        self.cf.CFSetGetValues(devices, array)
        self.device = array[0]
        self.devices = devices
        status = self.io.IOHIDDeviceOpen(self.device, 0)
        if status: raise RuntimeError(f'IOHIDDeviceOpen: {status:#x}')
        self.reports = []
        def received(context, result, sender, report_type, report_id, data, length):
            if not result:
                self.reports.append(c.string_at(data, length))
        self.callback = self.callback_type(received)
        self.buffer = c.create_string_buffer(512)
        self.mode = p.in_dll(self.cf, 'kCFRunLoopDefaultMode').value
        self.run_loop = self.cf.CFRunLoopGetCurrent()
        self.io.IOHIDDeviceRegisterInputReportCallback(self.device, self.buffer, 512, self.callback, None)
        self.io.IOHIDDeviceScheduleWithRunLoop(self.device, self.run_loop, self.mode)

    def close(self):
        self.io.IOHIDDeviceUnscheduleFromRunLoop(self.device, self.run_loop, self.mode)
        self.io.IOHIDDeviceClose(self.device, 0)
        self.io.IOHIDManagerClose(self.manager, 0)
        self.cf.CFRelease(self.devices)
        self.cf.CFRelease(self.manager)
        for value in self.match_values: self.cf.CFRelease(value)

    def send(self, report):
        if len(report) != 64: raise ValueError('expected a 64-byte report')
        data = c.create_string_buffer(report)
        status = self.io.IOHIDDeviceSetReport(self.device, 1, 0, data, 64)
        if status: raise RuntimeError(f'IOHIDDeviceSetReport: {status:#x}')

    def receive(self, timeout=3):
        end = time.monotonic() + timeout
        while not self.reports and time.monotonic() < end:
            self.cf.CFRunLoopRunInMode(self.mode, .01, True)
        if not self.reports: raise TimeoutError('No HID input report received')
        return self.reports.pop(0)

    def transact(self, cid, command, payload, timeout=3):
        packet = cid.to_bytes(4, 'big') + bytes([command]) + len(payload).to_bytes(2, 'big') + payload[:57]
        self.send(packet.ljust(64, b'\0'))
        at, sequence = 57, 0
        while at < len(payload):
            packet = cid.to_bytes(4, 'big') + bytes([sequence]) + payload[at:at+59]
            self.send(packet.ljust(64, b'\0'))
            at += 59; sequence += 1
        result, expected, sequence = bytearray(), None, 0
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            report = self.receive(max(.01, end - time.monotonic()))
            if len(report) != 64 or report[4] != 0xbb:
                print('IN', report.hex(), flush=True)
            if len(report) != 64 or report[:4] != cid.to_bytes(4, 'big'):
                continue
            cmd = report[4]
            if cmd == 0xbb: continue
            if cmd == 0xbf: raise RuntimeError(f'CTAPHID_ERROR: {report[7]:#x}')
            if expected is None:
                if cmd != command: raise RuntimeError(f'Unexpected command {cmd:#x}')
                expected = int.from_bytes(report[5:7], 'big')
                result += report[7:]
            else:
                if cmd != sequence: raise RuntimeError('Invalid continuation sequence')
                sequence += 1
                result += report[5:]
            if len(result) >= expected: return bytes(result[:expected])
        raise TimeoutError('CTAPHID transaction timed out')


def encode(value):
    def head(major, number):
        if number < 24: return bytes([(major << 5) | number])
        size = 1 if number <= 255 else 2 if number <= 65535 else 4
        return bytes([(major << 5) | (24 if size == 1 else 25 if size == 2 else 26)]) + number.to_bytes(size, 'big')
    if isinstance(value, bool): return b'\xf5' if value else b'\xf4'
    if isinstance(value, int): return head(0, value) if value >= 0 else head(1, -value - 1)
    if isinstance(value, bytes): return head(2, len(value)) + value
    if isinstance(value, str):
        data = value.encode('utf-8'); return head(3, len(data)) + data
    if isinstance(value, list): return head(4, len(value)) + b''.join(encode(item) for item in value)
    if isinstance(value, dict):
        pairs = [(encode(k), encode(v)) for k, v in value.items()]
        pairs.sort(key=lambda pair: (len(pair[0]), pair[0]))
        return head(5, len(pairs)) + b''.join(k + v for k, v in pairs)
    raise ValueError('Unsupported CBOR value')


def decode(data, at=0):
    initial = data[at]; at += 1
    major, number = initial >> 5, initial & 31
    if number >= 24:
        if number > 26: raise ValueError('Unsupported CBOR length')
        size = 1 << (number - 24)
        number = int.from_bytes(data[at:at+size], 'big'); at += size
    if major == 0: return number, at
    if major == 1: return -number - 1, at
    if major in (2, 3):
        value = data[at:at+number]
        if len(value) != number: raise ValueError('Truncated CBOR')
        return (value.decode('utf-8') if major == 3 else value), at + number
    if major == 4:
        result = []
        for _ in range(number):
            value, at = decode(data, at); result.append(value)
        return result, at
    if major == 5:
        result = {}
        for _ in range(number):
            key, at = decode(data, at); value, at = decode(data, at)
            result[key] = value
        return result, at
    if major == 7 and number in (20, 21): return number == 21, at
    raise ValueError('Unsupported CBOR value')


def exercise(hid, cid, output):
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.asymmetric import ec
    rp = 'ame-test.local'
    rp_hash = hashlib.sha256(rp.encode()).digest()
    request = {1: os.urandom(32), 2: {'id': rp},
               3: {'id': os.urandom(16), 'name': 'Ame hardware test'},
               4: [{'alg': -7, 'type': 'public-key'}]}
    print('Registration: release VOLUME UP, then press it when the phone prompts.', flush=True)
    result = hid.transact(cid, 0x90, b'\x01' + encode(request), 35)
    if not result or result[0]: raise RuntimeError(f'MakeCredential status: {result[:1].hex()}')
    registration, end = decode(result, 1)
    if end != len(result): raise RuntimeError('Trailing registration bytes')
    auth = registration[2]
    if auth[:32] != rp_hash or auth[32] != 0x41: raise RuntimeError('Invalid registration authenticator data')
    length = int.from_bytes(auth[53:55], 'big')
    credential = auth[55:55+length]
    cose, end = decode(auth, 55+length)
    if end != len(auth) or cose[1] != 2 or cose[3] != -7 or cose[-1] != 1:
        raise RuntimeError('Invalid ES256 public key')
    key = ec.EllipticCurvePublicNumbers(int.from_bytes(cose[-2], 'big'), int.from_bytes(cose[-3], 'big'), ec.SECP256R1()).public_key()
    record = {'rp': rp, 'id': credential.hex(), 'x': cose[-2].hex(), 'y': cose[-3].hex()}
    if output:
        Path(output).write_text(json.dumps(record, indent=2) + '\n')
    print('Registration OK. Release VOLUME UP before the next prompt.', flush=True)
    challenge = os.urandom(32)
    request = {1: rp, 2: challenge, 3: [{'id': credential, 'type': 'public-key'}]}
    print('Signing: press VOLUME UP for the sign-in prompt.', flush=True)
    result = hid.transact(cid, 0x90, b'\x02' + encode(request), 35)
    if not result or result[0]: raise RuntimeError(f'GetAssertion status: {result[:1].hex()}')
    assertion, end = decode(result, 1)
    auth = assertion[2]
    if end != len(result) or auth[:32] != rp_hash or auth[32] != 1 or assertion[1]['id'] != credential:
        raise RuntimeError('Invalid assertion authenticator data')
    key.verify(assertion[3], auth + challenge, ec.ECDSA(hashes.SHA256()))
    print('ES256 registration and physical-approval signing verified independently with cryptography.')

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ping', action='store_true')
    parser.add_argument('--pin-info', action='store_true', help='check PIN capabilities and key agreement without setting a PIN')
    parser.add_argument('--exercise', action='store_true', help='local ES256 registration and signing; requires two physical approvals')
    parser.add_argument('--output', help='save the local test credential and public key')
    args = parser.parse_args()
    hid = MacHid()
    try:
        nonce = os.urandom(8)
        result = hid.transact(0xffffffff, 0x86, nonce)
        if len(result) != 17 or result[:8] != nonce: raise RuntimeError('Invalid INIT response')
        cid = int.from_bytes(result[8:12], 'big')
        print(f'INIT channel={cid:#x} caps={result[16]:#x}', flush=True)
        result = hid.transact(cid, 0x90, b'\x04')
        if not result or result[0]: raise RuntimeError(f'GetInfo failed: {result.hex()}')
        print('GetInfo OK:', result.hex())
        if args.pin_info:
            info, _ = decode(result[1:])
            if info[6] != [1] or not info[4]['rk'] or 'clientPin' not in info[4]:
                raise RuntimeError('Missing PIN/discoverable credential capabilities')
            for sub, key in ((1, 3), (2, 1)):
                result = hid.transact(cid, 0x90, b'\x06' + encode({1: 1, 2: sub}))
                if not result or result[0]: raise RuntimeError(f'ClientPIN {sub} failed: {result.hex()}')
                values, _ = decode(result[1:])
                value = values[key]
                if sub == 1:
                    print(f'PIN configured={info[4]["clientPin"]}; retries={value}')
                elif value[1] != 2 or value[3] != -25 or value[-1] != 1 or len(value[-2]) != 32 or len(value[-3]) != 32:
                    raise RuntimeError('Invalid PIN key agreement')
            print('PIN protocol 1 key agreement OK')
        if args.exercise: exercise(hid, cid, args.output)
        if args.ping:
            data = bytes(range(256)) * 4
            if hid.transact(cid, 0x81, data) != data: raise RuntimeError('PING mismatch')
            print('PING 1024 bytes OK')
    finally:
        hid.close()


if __name__ == '__main__':
    main()
