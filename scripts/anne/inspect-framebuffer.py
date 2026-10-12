#!/usr/bin/env python3
"""Read the Huawei recovery fb0 ABI without changing display state.

The native Linux probe only opens fb0 read-only and issues FBIOGET ioctls.
Its executable is temporarily copied to TWRP's RAM-backed /tmp directory.
No framebuffer mappings, writes, FBIOPUT ioctls, or MMIO reads are performed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[2]
STOCK_SHA256 = "ad00caaa84c53d3ce478e562a6a1116ab720b2f0fce565c518928476b55cc268"


def decode(data):
    if len(data) != 272 or data[:4] != b"FBQ1" or struct.unpack_from("<I", data, 4)[0] != 1:
        raise ValueError("invalid AArch64 framebuffer probe report")
    opened, fixed_result, variable_result = struct.unpack_from("<3q", data, 8)
    result = {"open_result": opened, "fixed_result": fixed_result,
              "variable_result": variable_result}
    if opened < 0 or fixed_result != 0 or variable_result != 0:
        raise ValueError(f"framebuffer query failed: {result}")
    base = 32
    fixed = {"id": data[base:base + 16].split(b"\0", 1)[0].decode("ascii"),
             "smem_start": hex(struct.unpack_from("<Q", data, base + 16)[0])}
    for offset, name in zip((24, 28, 32, 36, 48, 64, 68),
                            ("smem_len", "type", "type_aux", "visual", "line_length", "mmio_len", "accel")):
        fixed[name] = struct.unpack_from("<I", data, base + offset)[0]
    fixed["mmio_start"] = hex(struct.unpack_from("<Q", data, base + 56)[0])
    for offset, name in zip((40, 42, 44, 72), ("xpanstep", "ypanstep", "ywrapstep", "capabilities")):
        fixed[name] = struct.unpack_from("<H", data, base + offset)[0]
    words = struct.unpack_from("<40I", data, 112)
    variable = dict(zip(("xres", "yres", "xres_virtual", "yres_virtual", "xoffset", "yoffset",
                         "bits_per_pixel", "grayscale"), words[:8]))
    for i, name in enumerate(("red", "green", "blue", "transp")):
        variable[name] = dict(zip(("offset", "length", "msb_right"), words[8 + 3 * i:11 + 3 * i]))
    variable.update(zip(("nonstd", "activate", "height_mm", "width_mm", "accel_flags", "pixclock_raw",
                         "left_margin", "right_margin", "upper_margin", "lower_margin", "hsync_len",
                         "vsync_len", "sync", "vmode", "rotate", "colorspace"), words[20:36]))
    result.update(fixed=fixed, variable=variable,
                  address_interpretation="unverified; vendor smem_start may be an IOVA")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, help="compiled fb-ioctl-probe.c AArch64 Linux executable")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--offline", action="store_true", help="decode an existing ioctl.bin capture")
    args = parser.parse_args()
    if args.offline:
        print(json.dumps(decode((args.output / "ioctl.bin").read_bytes()), indent=2))
        return
    if not args.probe or not args.probe.is_file():
        parser.error("--probe must identify the compiled AArch64 Linux probe")
    args.output.mkdir(parents=True, exist_ok=True)
    if (args.output / "ioctl.bin").exists():
        parser.error("ioctl.bin already exists; use a fresh output or --offline")
    serial = json.loads((ROOT / "out/anne-device-private/adb-targets.json").read_text())[0]["serial"]
    adb = [str(ROOT / "out/host-tools/platform-tools/adb"), "-s", serial]

    def run(*command, binary=False):
        completed = subprocess.run([*adb, *command], capture_output=True, timeout=15)
        if completed.returncode:
            error = completed.stderr.decode(errors="replace").replace(serial, "<device>")
            raise ValueError(f"ADB command failed ({completed.returncode}): {error[:1000]}")
        return completed.stdout if binary else completed.stdout.decode(errors="replace").replace(serial, "<device>").strip()

    if run("get-state") != "recovery":
        raise ValueError("expected the selected phone in recovery")
    identity = run("shell", "getprop ro.product.device; getprop ro.product.model; getprop ro.twrp.version").splitlines()
    if len(identity) != 3 or not identity[2].strip() or not (
            "ane" in identity[0].lower() or "ane" in identity[1].lower() or identity[1] == "P20 Lite"):
        raise ValueError("expected the Huawei anne phone in TWRP")
    checksum = run("shell", "sha256sum /dev/block/platform/hi_mci.0/by-name/kernel").split()[0]
    if checksum != STOCK_SHA256:
        raise ValueError("stock shared kernel checksum mismatch")
    remote = "/tmp/ame-fb-ioctl-probe"
    try:
        run("push", str(args.probe.resolve()), remote)
        run("shell", "chmod", "700", remote)
        data = run("exec-out", remote, binary=True)
        (args.output / "ioctl.bin").write_bytes(data)
        result = decode(data)
    finally:
        run("shell", "rm", "-f", remote)
    result.update(schema=1, target="huawei-anne", identity=identity,
                  stock_kernel_sha256=checksum,
                  probe_sha256=hashlib.sha256(args.probe.read_bytes()).hexdigest(),
                  capture_sha256=hashlib.sha256(data).hexdigest())
    (args.output / "ioctl.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    try:
        main()
    except (ValueError, subprocess.TimeoutExpired) as error:
        raise SystemExit(str(error))
