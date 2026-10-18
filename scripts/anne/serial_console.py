"""Raw CDC transport and terminal relay, independent of AMK1 key framing."""
from __future__ import annotations

import errno
import math
import os
import select
import sys
import termios
import time
import tty


def write_until(fd: int, data: bytes, deadline: float):
    view = memoryview(data)
    while view:
        remaining = deadline - time.monotonic()
        if remaining <= 0 or not select.select([], [fd], [], remaining)[1]:
            raise TimeoutError("USB serial write timed out")
        try:
            count = os.write(fd, view)
        except BlockingIOError:
            continue
        if not count:
            raise OSError(errno.ENODEV, "USB serial disconnected")
        view = view[count:]


class Serial:
    """Own a raw nonblocking serial FD and restore its settings on every exit."""
    def __init__(self, path: str):
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        self.previous = None
        try:
            self.previous = termios.tcgetattr(self.fd)
            tty.setraw(self.fd)
            attrs = termios.tcgetattr(self.fd)
            attrs[4] = attrs[5] = termios.B115200
            termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        except BaseException:
            self.close()
            raise

    def write(self, data: bytes, deadline: float):
        write_until(self.fd, data, deadline)

    def read(self, deadline: float) -> bytes:
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([self.fd], [], [], remaining)[0]:
                raise TimeoutError("USB serial read timed out")
            try:
                data = os.read(self.fd, 256)
            except BlockingIOError:
                continue
            if not data:
                raise OSError(errno.ENODEV, "USB serial disconnected")
            return data

    def close(self):
        if self.fd < 0:
            return
        fd, self.fd = self.fd, -1
        try:
            if self.previous is not None:
                try:
                    termios.tcsetattr(fd, termios.TCSANOW, self.previous)
                except (OSError, termios.error) as exc:
                    # macOS invalidates the old tty after reset/unplug. Cleanup
                    # must still close it without masking the original failure.
                    if not exc.args or exc.args[0] not in (errno.ENXIO, errno.ENODEV, errno.EIO, errno.EBADF):
                        raise
        finally:
            os.close(fd)


def relay(fd: int, input_fd: int, emit) -> int:
    """Full duplex with one bounded input chunk; read replies while TX blocks.

    select returns integer FDs here. Never call fileno() on a ready serial FD.
    Ctrl-] stops accepting input after the preceding bytes have been sent.
    """
    pending = memoryview(b"")
    exiting = False
    while not exiting or pending:
        readers = [fd] + ([input_fd] if not exiting and not pending else [])
        ready, writable, _ = select.select(readers, [fd] if pending else [], [], 30)
        if fd in ready:
            try:
                data = os.read(fd, 4096)
            except BlockingIOError:
                data = None
            if data == b"":
                raise OSError(errno.ENODEV, "USB serial disconnected")
            if data:
                emit(data)
        if input_fd in ready:
            data = os.read(input_fd, 4096)
            if not data:
                exiting = True
            elif b"\x1d" in data:
                data = data.split(b"\x1d", 1)[0]
                exiting = True
            pending = memoryview(data)
        if fd in writable and pending:
            try:
                count = os.write(fd, pending)
            except BlockingIOError:
                continue
            if not count:
                raise OSError(errno.ENODEV, "USB serial disconnected")
            pending = pending[count:]
    return 0


def console_mode(path: str, send: str | None, enter: bool, read_seconds: float) -> int:
    if not math.isfinite(read_seconds) or read_seconds < 0:
        raise ValueError("--read must be a finite nonnegative duration")
    if send is None and not sys.stdin.isatty():
        raise ValueError("Interactive console needs a terminal; use --send for a command")
    link = Serial(path)
    previous_input = None
    try:
        def emit(data):
            sys.stdout.buffer.write(data)
            sys.stdout.buffer.flush()

        if send is not None:
            link.write(send.encode() + (b"\r" if enter else b""), time.monotonic() + 3)
            deadline = time.monotonic() + read_seconds
            while time.monotonic() < deadline:
                if not select.select([link.fd], [], [], max(0, deadline - time.monotonic()))[0]:
                    break
                try:
                    data = os.read(link.fd, 4096)
                except BlockingIOError:
                    continue
                if not data:
                    break
                emit(data) # stream output without an unbounded accumulation buffer
            return 0
        print("Ame serial console; Ctrl-] exits.", file=sys.stderr)
        input_fd = sys.stdin.fileno()
        previous_input = termios.tcgetattr(input_fd)
        tty.setraw(input_fd)
        while True:
            try:
                return relay(link.fd, input_fd, emit)
            except OSError as exc:
                if exc.errno not in (errno.ENXIO, errno.ENODEV, errno.EIO, errno.EBADF):
                    raise
                link.close()
                print("\r\nUSB serial disconnected; waiting for " + path + " (Ctrl-] exits).", file=sys.stderr)
                while True:
                    # Discard keystrokes from the disconnected session. Never
                    # replay a command after an uncertain partial USB write.
                    if select.select([input_fd], [], [], 0.2)[0]:
                        data = os.read(input_fd, 4096)
                        if not data or b"\x1d" in data:
                            return 0
                    try:
                        link = Serial(path)
                        break
                    except (OSError, termios.error) as reconnect:
                        if not reconnect.args or reconnect.args[0] not in (errno.ENOENT, errno.ENXIO, errno.ENODEV, errno.EIO, errno.EBUSY):
                            raise
                print("\r\nUSB serial reconnected.", file=sys.stderr)
    finally:
        try:
            if previous_input is not None:
                termios.tcsetattr(sys.stdin.fileno(), termios.TCSADRAIN, previous_input)
        finally:
            link.close()
