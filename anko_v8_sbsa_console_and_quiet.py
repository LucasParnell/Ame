#!/usr/bin/env python3
from __future__ import annotations
import re
from pathlib import Path

BOOTARGS = "console=ttyAMA0,115200n8 earlycon=pl011,0x09000000 loglevel=4 nosmp maxcpus=1 cma=0 coherent_pool=1M cpuidle.off=1 rdinit=/init panic=-1"
UART_BLOCK = '''    uart0: serial@9000000 {
        compatible = "arm,sbsa-uart";
        reg = <0x0 0x09000000 0x0 0x1000>;
        interrupts = <0 1 4>;
        current-speed = <115200>;
        status = "okay";
    };'''

root = Path.cwd()

def rw(path: str, fn):
    p = root / path
    s = p.read_text()
    ns = fn(s)
    if ns != s:
        p.write_text(ns)
        print(f"updated {path}")
    else:
        print(f"unchanged {path}")

# Keep boot quiet and make the runtime console explicit.
def bootargs_edit(s: str) -> str:
    s = re.sub(r': "\$\{DANGO_LINUX_BOOTARGS:=.*?\}"', f': "${{DANGO_LINUX_BOOTARGS:={BOOTARGS}}}"', s)
    s = re.sub(r'bootargs = "[^"]*";', f'bootargs = "{BOOTARGS}";', s)
    return s

for path in [
    "scripts/linux/build-min-linux.sh",
    "scripts/linux/build-dtb.sh",
    "scripts/linux/run-qemu-linux.sh",
    "linux-artifacts/dts/dango-linux-minimal.dts",
]:
    p = root / path
    if p.exists():
        rw(path, bootargs_edit)

# Switch from PrimeCell PL011-on-AMBA to SBSA UART.  The existing virtual UART
# does not expose AMBA PrimeCell PID/CID registers, so Linux's AMBA PL011 probe
# will not create ttyAMA0.  The SBSA UART binding is the correct direct platform
# binding for a simple virtual UART and does not require AMBA identification.
def dts_edit(s: str) -> str:
    if 'stdout-path' not in s:
        s = s.replace('    chosen {\n', '    chosen {\n        stdout-path = "serial0:115200n8";\n', 1)

    # Drop optional v7 fixed-clock nodes if present.  SBSA UART does not need
    # AMBA clocks, and keeping phandles to removed properties is unnecessary.
    s = re.sub(r'\n    uartclk: uartclk \{\n(?:        .*\n)*?    \};\n', '\n', s)
    s = re.sub(r'\n    apb_pclk: apb-pclk \{\n(?:        .*\n)*?    \};\n', '\n', s)

    # Replace the whole serial@9000000 node, regardless of whether it currently
    # uses clock-frequency or clocks/clock-names.
    s, n = re.subn(r'    uart0: serial@9000000 \{\n(?:        .*\n)*?    \};', UART_BLOCK, s, count=1)
    if n != 1:
        raise SystemExit("error: could not find uart0: serial@9000000 node")
    return s

for path in [
    "linux-artifacts/dts/dango-linux-minimal.dts.in",
    "linux-artifacts/dts/dango-linux-minimal.dts",
]:
    p = root / path
    if p.exists():
        rw(path, dts_edit)

print("done")
