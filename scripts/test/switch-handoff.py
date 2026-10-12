#!/usr/bin/env python3
"""Run the actual BL33 probe in QEMU with DC registers backed by fixture RAM.

This validates CPU execution and memory writes, not Tegra hardware emulation.
Usage: python3 scripts/test/switch-handoff.py [build-switch]
"""
import json
from pathlib import Path
import select
import struct
import subprocess
import sys
import tempfile
import time


def command(name, arguments=None):
    message = {'execute': name}
    if arguments is not None:
        message['arguments'] = arguments
    return json.dumps(message).encode() + b'\n'


def check(image, valid):
    with tempfile.TemporaryDirectory(prefix='ame-handoff-') as tmp:
        root = Path(tmp)
        # Independent register fixture: packed 16x8 pixels, then unsupported
        # block-linear scanout. Non-default selectors must survive both paths.
        registers = bytearray(0x3000)
        values = {0x40: 4, 0x42: 0x20, 0x700: 1 << 30, 0x703: 0xc,
                  0x705: (8 << 16) | 16, 0x706: (8 << 16) | 64,
                  0x70a: 64, 0x800: 0x90000000, 0x80b: 0 if valid else 2}
        for index, value in values.items():
            struct.pack_into('<I', registers, index * 4, value)
        (root / 'dc.bin').write_bytes(registers)
        # These addresses fall inside virt's RAM, allowing the actual payload's
        # MMIO accesses to use the fixture without pretending to emulate Tegra.
        args = ['qemu-system-aarch64', '-machine', 'virt,virtualization=on',
                '-cpu', 'cortex-a57', '-accel', 'tcg', '-m', '3G', '-smp', '1',
                '-display', 'none', '-serial', 'none', '-monitor', 'none',
                '-S', '-qmp', 'stdio', '-device',
                f'loader,file={image},addr=0xaa000000,cpu-num=0,force-raw=on',
                '-device', f'loader,file={root}/dc.bin,addr=0x54200000,force-raw=on']
        with subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, bufsize=0) as process:
            try:
                def receive():
                    if not select.select([process.stdout], [], [], 10)[0]:
                        raise TimeoutError('QMP response')
                    return json.loads(process.stdout.readline())

                if 'QMP' not in receive():
                    raise RuntimeError('missing QMP greeting')
                for name in ('qmp_capabilities', 'cont'):
                    process.stdin.write(command(name))
                    while True:
                        reply = receive()
                        if 'error' in reply:
                            raise RuntimeError(reply['error'])
                        if 'return' in reply:
                            break
                time.sleep(0.5)
                requests = command('stop')
                for name, address, size in [('fb', 0x90000000, 516),
                                            ('selectors', 0x54200100, 12)]:
                    requests += command('pmemsave', {'val': address, 'size': size,
                                                    'filename': str(root / name)})
                requests += command('quit')
                output, errors = process.communicate(requests, timeout=15)
                if process.returncode:
                    raise RuntimeError(errors.decode())
                for line in output.splitlines():
                    reply = json.loads(line)
                    if 'error' in reply:
                        raise RuntimeError(reply['error'])
            finally:
                if process.poll() is None:
                    process.kill()
                    process.communicate()
        expected = struct.pack('<I', 0xffff00ff if valid else 0) * 128 + bytes(4)
        if (root / 'fb').read_bytes() != expected:
            raise RuntimeError('payload framebuffer write mismatch')
        selectors = (root / 'selectors').read_bytes()
        if struct.unpack_from('<I', selectors)[0] != 4 or \
                struct.unpack_from('<I', selectors, 8)[0] != 0x20:
            raise RuntimeError('payload did not restore DC selectors')


def main():
    build = Path(sys.argv[1] if len(sys.argv) > 1 else 'build-switch')
    image = (build / 'kernel/switch-handoff.bin').resolve()
    data = image.read_bytes()
    linked, offset = struct.unpack_from('<QQ', data, 8)
    if linked != 0xaa000000 or not 24 <= offset <= len(data) - 8 or \
            data[offset:offset + 8] != bytes(8):
        raise RuntimeError('invalid Hekate BL33 header/DTB sentinel')
    for valid in (True, False):
        check(image, valid)
    print('PASS: BL33 header, AArch64 execution, framebuffer bounds and DC selector restoration')


if __name__ == '__main__':
    main()
