#!/usr/bin/env python3
"""Upload and run an ephemeral native MXI app after `container start` in msh.

The CDC console remains available for msh and `diag log`. This tool claims
only vendor bulk interface 2. No file is written to the phone's storage.
"""
from __future__ import annotations
import argparse
from pathlib import Path
import struct
import sys
import time
import zlib
from usb_bulk import Bulk

MAGIC, ACK = b"AMK1", 0x8000
HELLO, BEGIN, DATA, RUN, POLL, CANCEL = range(0x100, 0x106)
CLOSED, READY, RECEIVING, RUNNING, STOPPING = range(5)
NO_EXIT = -2147483648
MAX_IMAGE = 8 * 1024 * 1024

def encode(sequence: int, kind: int, payload: bytes = b"") -> bytes:
    if not 0 < sequence <= 0xffffffff or len(payload) > 64:
        raise ValueError("invalid application frame bounds")
    head = MAGIC + struct.pack("<IHH", sequence, kind, len(payload))
    return head + struct.pack("<I", zlib.crc32(head + payload)) + payload

class Sender:
    def __init__(self, transport, timeout=5.0):
        self.transport, self.timeout = transport, timeout
        self.sequence, self.session = 0, None
        self.buffer = bytearray()

    def request(self, kind, payload=b""):
        if kind != HELLO:
            if self.session is None:
                raise RuntimeError("application session has not been opened")
            payload = struct.pack("<Q", self.session) + payload
        self.sequence += 1
        deadline = time.monotonic() + self.timeout
        self.transport.write(encode(self.sequence, kind, payload), deadline)
        while time.monotonic() < deadline:
            if len(self.buffer) >= 16:
                at = self.buffer.find(MAGIC)
                if at < 0:
                    del self.buffer[:-3]
                elif at:
                    del self.buffer[:at]
                if len(self.buffer) >= 16:
                    seq, response, size, crc = struct.unpack_from("<IHHI", self.buffer, 4)
                    if size > 64:
                        del self.buffer[0]
                        continue
                    if len(self.buffer) >= 16 + size:
                        frame = bytes(self.buffer[:16 + size])
                        del self.buffer[:16 + size]
                        if zlib.crc32(frame[:12] + frame[16:]) != crc:
                            continue
                        if seq != self.sequence or response != (kind | ACK):
                            continue
                        if size != 24:
                            raise RuntimeError("incompatible app-stream reply")
                        status, state, received, code, session = struct.unpack_from("<iIIiQ", frame, 16)
                        if status:
                            hint = "; run 'container start' in msh first" if status == -11 else ""
                            raise RuntimeError(f"Ame rejected request with status {status}{hint}")
                        if state > STOPPING or not session:
                            raise RuntimeError("invalid application session reply")
                        if self.session is None:
                            self.session = session
                        elif self.session != session:
                            raise RuntimeError("application session changed during transfer")
                        return state, received, code
            self.buffer.extend(self.transport.read(deadline))
            if len(self.buffer) > 4096:
                raise RuntimeError("unbounded app-stream reply")
        raise TimeoutError("application response timed out")

    def run(self, image: bytes, run_ms=30000):
        if not 128 <= len(image) <= MAX_IMAGE or image[:4] != b"MXI1":
            raise ValueError("expected a native MXI1 image between 128 bytes and 8 MiB")
        if not 1 <= run_ms <= 300000:
            raise ValueError("run timeout must be between 1 and 300000 ms")
        state, _, _ = self.request(HELLO)
        if state != READY:
            raise RuntimeError("container is busy; inspect 'container status' in msh")
        owned = False
        try:
            # BEGIN can reach the device even if its ACK is lost.
            owned = True
            self.request(BEGIN, struct.pack("<II", len(image), run_ms))
            for offset in range(0, len(image), 52):
                data = image[offset:offset + 52]
                state, received, _ = self.request(DATA, struct.pack("<I", offset) + data)
                if state != RECEIVING or received != offset + len(data):
                    raise RuntimeError("upload acknowledgement has an unexpected offset")
            self.request(RUN)
            deadline = time.monotonic() + run_ms / 1000 + 5
            while time.monotonic() < deadline:
                state, _, code = self.request(POLL)
                if state == READY and code != NO_EXIT:
                    return code
                if state == CLOSED:
                    raise RuntimeError("container closed while the application ran")
                time.sleep(0.02)
            raise TimeoutError("application did not finish cleanup")
        finally:
            if owned:
                try:
                    state, _, _ = self.request(CANCEL)
                    deadline = time.monotonic() + 5
                    while state == STOPPING and time.monotonic() < deadline:
                        time.sleep(0.02)
                        state, _, _ = self.request(POLL)
                    if state == STOPPING:
                        raise RuntimeError("application cleanup is still pending")
                except (OSError, RuntimeError, TimeoutError) as exc:
                    print(f"cleanup: {exc}; Ame also cleans up on USB disconnect or timeout", file=sys.stderr)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path, help="native .mxi app or test")
    parser.add_argument("--run-ms", type=int, default=30000)
    parser.add_argument("--vid", type=lambda s: int(s, 0), default=0x1209)
    parser.add_argument("--pid", type=lambda s: int(s, 0), default=0x0001)
    args = parser.parse_args()
    transport = None
    try:
        if args.image.stat().st_size > MAX_IMAGE:
            raise ValueError("image exceeds 8 MiB upload limit")
        image = args.image.read_bytes()
        transport = Bulk(args.vid, args.pid)
        code = Sender(transport).run(image, args.run_ms)
        print(f"app exit: {code}; app memory released")
        return code if 0 <= code <= 255 else 1
    except (OSError, ValueError, RuntimeError, TimeoutError) as exc:
        print(f"app-stream: {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130
    finally:
        if transport:
            transport.close()

if __name__ == "__main__":
    raise SystemExit(main())
