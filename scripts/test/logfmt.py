#!/usr/bin/env python3
"""Recover guest console output that QEMU interleaves with Ame's own log.

The appliance guest UART and the Ame host console share one QEMU serial file,
and the guest driver writes byte-by-byte, so an Ame log entry can land in the
middle of a guest line:

    dango: DRM card[    5.128952] [I] cpu1 kernel: guest uart hello
    0 driver=virtio_gpu dumb_buffer=1

Every Ame entry matches ``[<early|N.NNNNNN>] [<LEVEL>] ...\\n``. Removing those
entries *including their newline* rejoins the guest fragments, so a guest marker
can be matched regardless of how it was split. Ame markers are still found in
the raw stream, so callers should match against ``marker_stream`` (raw + guest).
"""
import re

# Ame log entry: "[    5.128952] [I] ..." or "[early] [I] ..." anywhere in the
# stream (it may be embedded mid-line), terminated by its own newline.
_AME_ENTRY = re.compile(r'\[(?:early|\s*\d+\.\d+)\] \[[A-Z]\] [^\n]*\n')


def normalize_guest(text):
    """Return `text` with Ame log entries removed and guest fragments rejoined."""
    return _AME_ENTRY.sub('', text)


def marker_stream(text):
    """Raw stream plus the de-interleaved guest view, for marker matching."""
    return text + '\n' + normalize_guest(text)
