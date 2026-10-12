import binascii
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SwitchBoot(unittest.TestCase):
    def test_legacy_script_framing_and_checksums(self):
        source = ROOT / 'boards/switch-t210/boot.cmd'
        with tempfile.TemporaryDirectory(prefix='ame-switch-boot-') as tmp:
            image = Path(tmp) / 'boot.scr'
            subprocess.run([sys.executable, str(ROOT / 'scripts/mk-uimage.py'),
                            '--input', str(source), '--output', str(image),
                            '--load', '0', '--entry', '0', '--type', 'script'], check=True)
            data = image.read_bytes()
        header = bytearray(data[:64])
        magic, hcrc, _, size, load, entry, dcrc, os_id, arch, kind, comp, _ = \
            struct.unpack('>7I4B32s', header)
        self.assertEqual((magic, os_id, arch, kind, comp), (0x27051956, 5, 22, 6, 0))
        self.assertEqual((load, entry, size), (0, 0, len(data) - 64))
        struct.pack_into('>I', header, 4, 0)
        self.assertEqual(binascii.crc32(header) & 0xffffffff, hcrc)
        self.assertEqual(binascii.crc32(data[64:]) & 0xffffffff, dcrc)
        # Independent decoding of the component table consumed by U-Boot source().
        length, terminator = struct.unpack_from('>II', data, 64)
        self.assertEqual(terminator, 0)
        self.assertEqual(data[72:72 + length], source.read_bytes())
        self.assertEqual(72 + length, len(data))

    def test_each_failed_load_prevents_handoff(self):
        # Exercise the actual hush-compatible script with instrumented commands.
        # No kernel handoff is allowed with a missing kernel, bundle or DTB.
        harness = '''
setenv() { :; }
load() {
    count=$((count + 1))
    echo "LOAD:$count:$4:$5"
    test "$count" != "$fail_at"
}
bootm() { echo "HANDOFF:$*"; exit 0; }
devtype=mmc devnum=1 distro_bootpart=1
kernel_addr_r=0x84000000 ramdisk_addr_r=0x92000000 fdt_addr_r=0x8d000000
count=0
fail_at=$1
. "$2"
'''
        for failed in range(4):
            with self.subTest(failed=failed):
                result = subprocess.run(['sh', '-c', harness, 'switch-boot', str(failed),
                                         str(ROOT / 'boards/switch-t210/boot.cmd')],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 1 if failed else 0, result.stderr)
                self.assertEqual('HANDOFF:' in result.stdout, failed == 0)
                self.assertEqual(result.stdout.count('LOAD:'), failed or 3)
                if not failed:
                    self.assertIn('HANDOFF:0x84000000 0x92000000 0x8d000000', result.stdout)


if __name__ == '__main__':
    unittest.main()
