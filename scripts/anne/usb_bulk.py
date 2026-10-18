"""Shared host transport for Ame's vendor USB bulk interface."""
import time

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

