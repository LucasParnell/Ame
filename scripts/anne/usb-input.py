#!/usr/bin/env python3
"""Talk to Ame's development USB endpoints from a Mac.

`console` is the raw serial console served by dev.serial: the CDC serial port
carries the Midix shell's input and output byte streams, so `screen
/dev/cu.usbmodem*` works the same way (stdlib only, needs no PyUSB). The framed
keycode commands (`text`, `key`, `interactive`, `release`) inject keyboard
usages through the bulk function (--bulk, needs PyUSB and libusb) and wait for
an ACK that proves ui.input's producer ring accepted each event. Framing does
not turn the Mac USB controller into a HID peripheral; Ame's separate gadget
receives bytes.
"""
from __future__ import annotations

import argparse
import glob
from serial_console import Serial, console_mode
import os
import select
import struct
import sys
import termios
import time
import tty
import zlib

MAGIC = b"AMK1"
HELLO, KEY, RELEASE, ACK = 1, 2, 3, 0x8000
MAX_PAYLOAD = 64
KEYS = {
    "enter": 0x28, "escape": 0x29, "esc": 0x29, "backspace": 0x2A,
    "tab": 0x2B, "space": 0x2C, "capslock": 0x39, "home": 0x4A,
    "end": 0x4D, "delete": 0x4C, "right": 0x4F, "left": 0x50,
    "down": 0x51, "up": 0x52,
}
PLAIN = "abcdefghijklmnopqrstuvwxyz1234567890\n\x1b\b\t -=[]\\\x00;'`,./"
SHIFT = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\n\x1b\b\t _+{}|\x00:\"~<>?"


def encode(sequence: int, kind: int, payload: bytes = b"") -> bytes:
    if not 0 < sequence <= 0xFFFFFFFF or len(payload) > MAX_PAYLOAD:
        raise ValueError("invalid frame bounds")
    header = MAGIC + struct.pack("<IHH", sequence, kind, len(payload))
    return header + struct.pack("<I", zlib.crc32(header + payload)) + payload


class Bulk:
    def __init__(self, vid: int, pid: int):
        try:
            import usb.core
            import usb.util
        except ImportError as exc:
            raise RuntimeError("Bulk needs PyUSB and libusb; CDC needs neither") from exc
        self.util = usb.util
        matches = list(usb.core.find(find_all=True, idVendor=vid, idProduct=pid))
        if len(matches) != 1:
            raise RuntimeError(f"Expected one Ame test device, found {len(matches)}")
        self.device = matches[0]
        configuration = self.device.get_active_configuration()
        interface = configuration[(2, 0)]
        if interface.bInterfaceClass != 0xFF:
            raise RuntimeError("Interface 2 is not the expected vendor bulk function")
        self.util.claim_interface(self.device, 2)

    def write(self, data: bytes, deadline: float):
        timeout = max(1, int((deadline - time.monotonic()) * 1000))
        if self.device.write(4, data, timeout) != len(data):
            raise OSError("Short USB bulk write")

    def read(self, deadline: float) -> bytes:
        timeout = max(1, int((deadline - time.monotonic()) * 1000))
        return bytes(self.device.read(0x85, 512, timeout))

    def close(self):
        self.util.release_interface(self.device, 2)
        self.util.dispose_resources(self.device)


class Sender:
    def __init__(self, transport, timeout: float = 3):
        self.transport, self.timeout = transport, timeout
        self.sequence = 0
        self.received = bytearray()

    def request(self, kind: int, payload: bytes = b""):
        self.sequence += 1
        deadline = time.monotonic() + self.timeout
        self.transport.write(encode(self.sequence, kind, payload), deadline)
        while True:
            if len(self.received) >= 16:
                at = self.received.find(MAGIC)
                if at < 0:
                    del self.received[:-3]
                elif at:
                    del self.received[:at]
                if len(self.received) >= 16:
                    sequence, response, size, crc = struct.unpack_from("<IHHI", self.received, 4)
                    if size > MAX_PAYLOAD:
                        del self.received[0]
                        continue
                    if len(self.received) >= 16 + size:
                        frame = bytes(self.received[:16 + size])
                        del self.received[:16 + size]
                        if zlib.crc32(frame[:12] + frame[16:]) != crc:
                            continue
                        if sequence == self.sequence and response == kind | ACK and size == 4:
                            status, = struct.unpack_from("<i", frame, 16)
                            if status:
                                raise RuntimeError(f"Ame rejected input with status {status}")
                            return
                        continue
            self.received.extend(self.transport.read(deadline))

    def key(self, usage: int, modifiers: int = 0):
        self.request(KEY, struct.pack("<HBB", usage, modifiers, 1))
        self.request(KEY, struct.pack("<HBB", usage, 0, 0))

    def character(self, character: str):
        if character == "\r":
            character = "\n"
        if character == "\x7f":
            character = "\b"
        if character in PLAIN and character != "\x00":
            self.key(PLAIN.index(character) + 4)
        elif character in SHIFT and character != "\x00":
            self.key(SHIFT.index(character) + 4, 2)
        elif 1 <= ord(character) <= 26:
            self.key(ord(character) + 3, 1)
        else:
            raise ValueError(f"US keyboard input cannot encode {character!r}")


def interactive(sender: Sender):
    if not sys.stdin.isatty():
        raise ValueError("Interactive mode needs a terminal")
    print("Sending keys to the phone; Ctrl-] exits. Read command output on the phone.", file=sys.stderr)
    previous = termios.tcgetattr(sys.stdin.fileno())
    try:
        tty.setraw(sys.stdin.fileno())
        escapes = {b"[A": "up", b"[B": "down", b"[C": "right", b"[D": "left",
                   b"[H": "home", b"[F": "end", b"[3~": "delete"}
        while True:
            byte = os.read(sys.stdin.fileno(), 1)
            if byte == b"\x1d":
                return
            if byte == b"\x1b":
                suffix = bytearray()
                while len(suffix) < 4 and select.select([sys.stdin], [], [], 0.03)[0]:
                    suffix.extend(os.read(sys.stdin.fileno(), 1))
                if bytes(suffix) in escapes:
                    sender.key(KEYS[escapes[bytes(suffix)]])
                else:
                    sender.key(KEYS["escape"])
                continue
            sender.character(byte.decode("ascii"))
    finally:
        termios.tcsetattr(sys.stdin.fileno(), termios.TCSADRAIN, previous)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    transport = parser.add_mutually_exclusive_group()
    transport.add_argument("--serial", help="Ame's /dev/cu.usbmodem device")
    transport.add_argument("--bulk", action="store_true", help="Use optional vendor bulk interface")
    parser.add_argument("--vid", type=lambda s: int(s, 0), default=0x1209)
    parser.add_argument("--pid", type=lambda s: int(s, 0), default=0x0001)
    sub = parser.add_subparsers(dest="command", required=True)
    text = sub.add_parser("text"); text.add_argument("text"); text.add_argument("--enter", action="store_true")
    key = sub.add_parser("key"); key.add_argument("key", help="Named key, character or ctrl+letter")
    sub.add_parser("interactive")
    sub.add_parser("release")
    console = sub.add_parser("console", help="Raw serial console for the Midix shell")
    console.add_argument("--send", help="Send literal text instead of interacting; prints output")
    console.add_argument("--enter", action="store_true", help="Append a carriage return to --send")
    console.add_argument("--read", type=float, default=3.0,
                         help="Seconds of output to print after --send")
    args = parser.parse_args()
    link = None
    try:
        if args.command == "console":
            if args.bulk:
                raise ValueError("console uses the CDC serial port; omit --bulk")
            path = args.serial
            if not path:
                paths = glob.glob("/dev/cu.usbmodem*")
                if len(paths) != 1:
                    raise ValueError("Use --serial with Ame's USB serial device")
                path = paths[0]
            return console_mode(path, args.send, args.enter, args.read)
        if not args.bulk:
            raise ValueError(
                "keycode framing moved to --bulk; the CDC serial port now serves "
                "the raw shell console — use `console` or screen")
        link = Bulk(args.vid, args.pid)
        sender = Sender(link)
        sender.request(HELLO)
        if args.command == "text":
            for character in args.text:
                sender.character(character)
            if args.enter:
                sender.key(KEYS["enter"])
        elif args.command == "key":
            name = args.key.lower()
            if name in KEYS:
                sender.key(KEYS[name])
            elif len(name) == 6 and name.startswith("ctrl+") and "a" <= name[-1] <= "z":
                sender.key(ord(name[-1]) - ord("a") + 4, 1)
            elif len(args.key) == 1:
                sender.character(args.key)
            else:
                raise ValueError("Unknown key")
        elif args.command == "interactive":
            interactive(sender)
        sender.request(RELEASE)
        return 0
    except (OSError, RuntimeError, TimeoutError, ValueError, UnicodeError) as exc:
        print(str(exc), file=sys.stderr)
        return 1
    finally:
        if link:
            link.close()


if __name__ == "__main__":
    raise SystemExit(main())
