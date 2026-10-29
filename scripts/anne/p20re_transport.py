"""ANE USB transports adapted from p20re's anne-el2/run.py and upload_probe.py.

Firmware assets remain external. Factory commands use 64-byte packets;
eMMC captures use 128 KiB requests, matching the validated handset procedure.
"""
import binascii
import struct
import time


class Factory:
    def __init__(self, serial):
        self.serial = serial

    def __enter__(self):
        import usb.core
        import usb.util
        self.util = usb.util
        devices = [d for d in usb.core.find(find_all=True, idVendor=0x18d1, idProduct=0xd00d)
                   if d.serial_number == self.serial]
        if len(devices) != 1:
            raise RuntimeError('Expected the confirmed ANE factory fastboot')
        self.d = devices[0]
        self.i = next(i for i in self.d.get_active_configuration()
                      if (i.bInterfaceClass, i.bInterfaceSubClass, i.bInterfaceProtocol) == (255, 66, 3))
        self.rd = next(e for e in self.i if e.bEndpointAddress & 128)
        self.wr = next(e for e in self.i if not e.bEndpointAddress & 128)
        usb.util.claim_interface(self.d, self.i.bInterfaceNumber)
        return self

    def __exit__(self, *args):
        import usb.core
        try:
            self.util.release_interface(self.d, self.i.bInterfaceNumber)
        except usb.core.USBError as error:
            # OEM boot disconnects the factory interface after its ACK.
            if error.errno != 19:
                raise
        finally:
            self.util.dispose_resources(self.d)

    def status(self):
        info = []
        for _ in range(128):
            p = bytes(self.rd.read(64, timeout=15000))
            if p[:4] == b'INFO':
                info.append(p[4:].rstrip(b'\0'))
                continue
            if p[:4] != b'OKAY':
                raise RuntimeError(f'Factory command failed: {p!r}')
            return b'\n'.join(info + [p[4:].rstrip(b'\0')])
        raise RuntimeError('Too many INFO packets')

    def command(self, command):
        # This factory receiver rearms after transmitting the previous status.
        # Leave time for its polling loop before submitting another command.
        time.sleep(.2)
        if isinstance(command, str):
            command = command.encode()
        if len(command) > 64:
            raise RuntimeError('Factory command exceeds 64 bytes')
        command = command.ljust(64, b'\0')
        if self.wr.write(command, timeout=5000) != len(command):
            raise RuntimeError('Short command write')
        return self.status()

    def read(self, kind, address, size):
        if self.command(f'upload_{kind}:{address:x}:{size:x}').strip():
            raise RuntimeError('Unexpected upload status payload')
        data = bytearray()
        while len(data) < size:
            p = bytes(self.rd.read(min(32768, size - len(data)), timeout=15000))
            if not p:
                raise RuntimeError('Short upload read')
            data += p
        if self.status().strip():
            raise RuntimeError('Unexpected upload final status')
        return bytes(data)

    def backup_emmc(self, address, size, path):
        data = bytearray()
        with path.open('wb') as f:
            for offset in range(0, size, 0x20000):
                block = self.read('emmc', address + offset, min(0x20000, size - offset))
                f.write(block)
                data += block
        return bytes(data)


def upload(port, label, blob, address):
    def frame(body, stage):
        packet = body + binascii.crc_hqx(body, 0).to_bytes(2, "big")
        written = port.write(packet)
        if written != len(packet):
            raise RuntimeError(f"{label} {stage}: short serial write")
        port.flush()
        reply = port.read(1)
        if reply != b"\xaa":
            detail = reply.hex() if reply else "timeout/disconnect"
            raise RuntimeError(f"{label} {stage}: expected ACK aa, received {detail}; stopping")

    print(f"Uploading {label}: {len(blob)} bytes to IDT address {address:#010x}", flush=True)
    frame(b"\xfe\x00\xff\x01" + struct.pack(">II", len(blob), address), "header")
    sequence = 1
    for offset in range(0, len(blob), 1024):
        seq = sequence & 255
        frame(bytes((0xda, seq, seq ^ 255)) + blob[offset:offset + 1024],
              f"data frame {sequence}")
        sequence += 1
    seq = sequence & 255
    frame(bytes((0xed, seq, seq ^ 255)), "tail")
    print(f"{label}: IDT frames acknowledged (does not prove execution)", flush=True)
    time.sleep(0.5)
