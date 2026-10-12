#!/usr/bin/env python3
"""Collect read-only TWRP observations or summarize a saved anne inspection."""

import argparse
from datetime import datetime, timezone
import gzip
import hashlib
import io
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib


ROOT = Path(__file__).resolve().parents[2]
MAX_TEXT = 1024 * 1024
MAX_ARTIFACT = 8 * 1024 * 1024
MAX_CONFIG = 4 * 1024 * 1024
PROPERTIES = (
    "ro.product.model", "ro.product.device", "ro.product.name", "ro.hardware",
    "ro.build.display.id", "ro.build.version.release", "ro.build.version.emui",
    "ro.twrp.version", "twrp.version", "ro.bootmode", "ro.boot.flash.locked",
    "ro.boot.verifiedbootstate",
)
# These commands only observe the running recovery. Never mount, reboot, root,
# load a kernel, read raw partition contents, or write anything on the phone.
OBSERVATIONS = {
    "kernel": ("uname", "-a"),
    "architecture": ("uname", "-m"),
    "shell_identity": ("id",),
    "cmdline": ("cat", "/proc/cmdline"),
    "partitions": ("cat", "/proc/partitions"),
    "iomem": ("cat", "/proc/iomem"),
    "interrupts": ("cat", "/proc/interrupts"),
    "cpuinfo": ("cat", "/proc/cpuinfo"),
    "input_devices": ("cat", "/proc/bus/input/devices"),
    "by_name": ("ls", "-l", "/dev/block/by-name"),
    "hi_mci_by_name": ("ls", "-l", "/dev/block/platform/hi_mci.0/by-name"),
    "recovery_fstab": ("cat", "/etc/recovery.fstab"),
    "twrp_fstab": ("cat", "/etc/twrp.fstab"),
    "kexec_tool": ("command", "-v", "kexec"),
    "kexec_loaded": ("cat", "/sys/kernel/kexec_loaded"),
    "kexec_disabled": ("cat", "/proc/sys/kernel/kexec_load_disabled"),
    "framebuffer_name": ("cat", "/sys/class/graphics/fb0/name"),
    "framebuffer_size": ("cat", "/sys/class/graphics/fb0/virtual_size"),
    "framebuffer_stride": ("cat", "/sys/class/graphics/fb0/stride"),
    "framebuffer_bpp": ("cat", "/sys/class/graphics/fb0/bits_per_pixel"),
    "framebuffer_modes": ("cat", "/sys/class/graphics/fb0/modes"),
    "backlight": ("ls", "/sys/class/backlight"),
    "power_supplies": ("ls", "/sys/class/power_supply"),
    "thermal_zones": ("ls", "/sys/class/thermal"),
}
ARTIFACTS = {
    "fdt": ("/sys/firmware/fdt", "firmware.dtb"),
    "kernel_config": ("/proc/config.gz", "config.gz"),
}
CONFIG_OPTIONS = (
    "CONFIG_ARM64", "CONFIG_KEXEC", "CONFIG_KEXEC_FILE", "CONFIG_KEXEC_CORE",
    "CONFIG_KEXEC_SIG_FORCE", "CONFIG_IKCONFIG_PROC", "CONFIG_KVM",
    "CONFIG_ARM_GIC", "CONFIG_ARM_PSCI_FW", "CONFIG_FB", "CONFIG_DRM",
)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def redact(text, serial=""):
    if serial:
        text = text.replace(serial, "<device-serial>")
    return re.sub(
        r"(?i)\b((?:androidboot\.)?(?:serialno|serial|imei|meid|android_id|"
        r"macaddr|macaddress|wlan_mac|bt_mac|uniqueid))=([^\s]+)",
        r"\1=<redacted>", text)


def parse_devices(text):
    devices = []
    for line in text.splitlines():
        if not line.strip() or line.startswith(("List of devices", "*")):
            continue
        fields = line.split()
        if len(fields) >= 2:
            devices.append((fields[0], fields[1]))
    return devices


def select_device(devices, serial=None):
    if serial is not None:
        matches = [item for item in devices if item[0] == serial]
        if len(matches) != 1:
            raise ValueError("the requested ADB device is not uniquely available")
    else:
        if not devices:
            raise ValueError("no ADB device found; connect the phone in TWRP")
        if len(devices) != 1:
            raise ValueError("multiple ADB devices found; select one with --serial")
        matches = devices
    selected, state = matches[0]
    if state not in ("device", "recovery"):
        raise ValueError(f"ADB device state is {state}; a working TWRP shell is required")
    return selected, state


def identify(properties, state):
    model = properties.get("ro.product.model", "").upper()
    device = properties.get("ro.product.device", "").lower()
    if not re.fullmatch(r"ANE-[A-Z0-9]+", model) and device != "anne":
        raise ValueError("device does not report an ANE model or anne codename; refusing inspection")
    twrp = properties.get("ro.twrp.version") or properties.get("twrp.version")
    if not twrp:
        raise ValueError("TWRP version is unavailable; boot the installed TWRP before collection")
    if state not in ("recovery", "device"):
        raise ValueError("a usable recovery ADB transport is required")
    return {
        "model": properties.get("ro.product.model") or None,
        "device": properties.get("ro.product.device") or None,
        "twrp_version": twrp,
        "status": "reported-anne-in-twrp",
    }


def validate_fdt(data):
    if len(data) < 40:
        raise ValueError("FDT header is missing")
    magic, size, off_struct, off_strings, off_reserve, version, compatible, _, \
        strings_size, struct_size = struct.unpack_from(">10I", data)
    if magic != 0xD00DFEED or size < 40 or size > len(data):
        raise ValueError("invalid FDT magic or total size")
    if version < 17 or compatible > 17:
        raise ValueError("unsupported FDT header version")
    for offset, extent in ((off_struct, struct_size), (off_strings, strings_size)):
        if offset < 40 or offset + extent > size:
            raise ValueError("FDT block exceeds the declared image")
    if off_struct % 4 or off_reserve % 8 or off_reserve < 40 or off_reserve + 16 > size:
        raise ValueError("invalid FDT block alignment or reservation offset")


def config_text(data):
    try:
        with gzip.GzipFile(fileobj=io.BytesIO(data)) as stream:
            decoded = stream.read(MAX_CONFIG + 1)
    except (OSError, EOFError, zlib.error) as error:
        raise ValueError("kernel config is not a valid gzip stream") from error
    if len(decoded) > MAX_CONFIG:
        raise ValueError("decompressed kernel config exceeds the size limit")
    try:
        text = decoded.decode("utf-8")
    except UnicodeDecodeError as error:
        raise ValueError("kernel config is not UTF-8") from error
    if not re.search(r"(?m)^(?:CONFIG_[A-Z0-9_]+=|# CONFIG_[A-Z0-9_]+ is not set)", text):
        raise ValueError("kernel config contains no configuration entries")
    return text


def config_options(text):
    result = dict.fromkeys(CONFIG_OPTIONS)
    for line in text.splitlines():
        match = re.fullmatch(r"(CONFIG_[A-Z0-9_]+)=(y|m|n)", line)
        disabled = re.fullmatch(r"# (CONFIG_[A-Z0-9_]+) is not set", line)
        if match and match[1] in result:
            result[match[1]] = match[2]
        elif disabled and disabled[1] in result:
            result[disabled[1]] = "n"
    return result


class Adb:
    def __init__(self, executable, timeout=8):
        self.executable = executable
        self.timeout = timeout
        self.serial = None

    def run(self, args):
        command = [self.executable]
        if self.serial is not None:
            command += ["-s", self.serial]
        return subprocess.run(command + list(args), capture_output=True,
                              timeout=self.timeout, check=False)


def capture(adb, output, name, command, binary=False):
    limit = MAX_ARTIFACT if binary else MAX_TEXT
    entry = {"command": list(command), "status": "unavailable"}
    try:
        result = adb.run(command)
    except subprocess.TimeoutExpired:
        return {**entry, "status": "timeout"}
    entry["returncode"] = result.returncode
    error = redact(result.stderr.decode("utf-8", errors="replace"), adb.serial)
    if error:
        entry["error"] = error[:2048]
    if result.returncode != 0:
        return entry
    if len(result.stdout) > limit:
        return {**entry, "status": "oversized"}
    data = result.stdout if binary else redact(
        result.stdout.decode("utf-8", errors="replace"), adb.serial).encode("utf-8")
    if binary:
        try:
            if name == "firmware.dtb":
                validate_fdt(data)
            else:
                config_text(data)
        except ValueError as error:
            return {**entry, "status": "invalid", "error": str(error)}
    path = output / "observations" / name
    path.write_bytes(data)
    return {**entry, "status": "ok", "path": str(path.relative_to(output)),
            "bytes": len(data), "sha256": sha256(data)}


def load_capture(root, entry):
    if entry.get("status") != "ok":
        return None
    relative = Path(entry["path"])
    path = (root / relative).resolve()
    if relative.is_absolute() or not path.is_relative_to(root.resolve()):
        raise ValueError("capture path leaves the inspection directory")
    if path.stat().st_size > MAX_ARTIFACT:
        raise ValueError("saved capture exceeds the size limit")
    data = path.read_bytes()
    if len(data) != entry["bytes"] or sha256(data) != entry["sha256"]:
        raise ValueError(f"capture integrity check failed: {relative}")
    return data


def summarize(report, root):
    if not isinstance(report, dict) or report.get("schema") != 1 or report.get("target") != "huawei-anne":
        raise ValueError("expected an anne inspection report with schema 1")
    for name in ("properties", "observations", "artifacts"):
        if not isinstance(report.get(name), dict):
            raise ValueError(f"report {name} must be an object")
    if any(not isinstance(value, str) for value in report["properties"].values()):
        raise ValueError("report properties must contain strings")
    for name in ("observations", "artifacts"):
        if any(not isinstance(entry, dict) for entry in report[name].values()):
            raise ValueError(f"report {name} entries must be objects")
    identity = identify(report["properties"], report["transport_state"])
    # Check every successful capture before interpreting any saved evidence.
    observations = {name: load_capture(root, entry)
                    for name, entry in report["observations"].items()}
    artifacts = {name: load_capture(root, entry)
                 for name, entry in report["artifacts"].items()}
    architecture = (observations.get("architecture") or b"").decode().strip() or None
    config = artifacts.get("kernel_config")
    options = config_options(config_text(config)) if config is not None else dict.fromkeys(CONFIG_OPTIONS)
    if artifacts.get("fdt") is not None:
        validate_fdt(artifacts["fdt"])
    kexec = options["CONFIG_KEXEC"]
    file_kexec = options["CONFIG_KEXEC_FILE"]
    if kexec == "y" or file_kexec == "y":
        support = "configured-not-tested"
    elif kexec == "n" and file_kexec == "n":
        support = "disabled-in-captured-config"
    else:
        support = "unknown"
    blocks = ["native handoff has not been tested", "recovery return path has not been tested",
              "physical image placement and hardware addresses are not verified"]
    if architecture != "aarch64":
        blocks.append("AArch64 recovery kernel has not been established")
    if artifacts.get("fdt") is None:
        blocks.append("running DTB is unavailable")
    if support != "configured-not-tested":
        blocks.append("recovery kexec support is " + support)
    disabled = (observations.get("kexec_disabled") or b"").strip()
    if disabled == b"1":
        blocks.append("running kernel reports kexec loading disabled")
    return {
        "identity": identity, "architecture": architecture, "config": options,
        "kexec": {"support": support,
                  "tool_path": (observations.get("kexec_tool") or b"").decode().strip() or None,
                  "load_disabled": {b"0": False, b"1": True}.get(disabled)},
        "fdt_captured": artifacts.get("fdt") is not None,
        "native_boot": {"ready": False, "blockers": blocks},
    }


def markdown(summary):
    identity, kexec = summary["identity"], summary["kexec"]
    lines = ["# Huawei anne recovery inspection", "",
             f"- Reported model: {identity['model'] or 'unknown'}",
             f"- Reported device: {identity['device'] or 'unknown'}",
             f"- TWRP: {identity['twrp_version']}",
             f"- Kernel architecture: {summary['architecture'] or 'unknown'}",
             f"- Running DTB captured: {'yes' if summary['fdt_captured'] else 'no'}",
             f"- Kexec kernel support: {kexec['support']}",
             f"- Kexec tool: {kexec['tool_path'] or 'not observed'}", "",
             "Native boot remains pending:", ""]
    lines += [f"- {block}" for block in summary["native_boot"]["blockers"]]
    lines += ["", "Collection only reads the recovery environment. Configured kexec is",
              "not proof of a usable handoff or EL2 ownership. No boot addresses",
              "are inferred from this report.", "",
              "Text captures redact the selected transport serial and common command-line",
              "identifiers. Other identifiers may remain; raw DTB/config captures are",
              "private device artifacts. Review files before sharing them.", ""]
    return "\n".join(lines)


def collect(adb, output=None, serial=None):
    devices = adb.run(("devices",))
    if devices.returncode:
        raise ValueError("ADB could not enumerate devices")
    adb.serial, state = select_device(parse_devices(devices.stdout.decode()), serial)
    properties = {}
    for name in PROPERTIES:
        result = adb.run(("shell", "getprop", name))
        properties[name] = redact(result.stdout.decode("utf-8", errors="replace").strip(), adb.serial) \
            if result.returncode == 0 else ""
    identify(properties, state)
    if output is None:
        output = Path(tempfile.mkdtemp(prefix="ame-anne-inspection-"))
    else:
        output = output.expanduser().resolve()
        output.mkdir(mode=0o700, parents=True, exist_ok=False)
    (output / "observations").mkdir(mode=0o700)
    report = {"schema": 1, "target": "huawei-anne", "operation": "read-only",
              "collected_utc": datetime.now(timezone.utc).isoformat(),
              "transport_state": state, "properties": properties,
              "observations": {}, "artifacts": {}}
    for name, command in OBSERVATIONS.items():
        report["observations"][name] = capture(adb, output, name + ".txt", ("shell",) + command)
    for name, (source, filename) in ARTIFACTS.items():
        report["artifacts"][name] = capture(adb, output, filename, ("exec-out", "cat", source), True)
    report["summary"] = summarize(report, output)
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    (output / "hardware.md").write_text(markdown(report["summary"]), encoding="utf-8")
    return output


def unique_keys(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate report key: {key}")
        result[key] = value
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="operation", required=True)
    live = commands.add_parser("collect", help="read a connected anne phone in TWRP")
    live.add_argument("--adb", help="ADB executable; otherwise use PATH or out/host-tools")
    live.add_argument("--serial", help="choose a device when multiple transports exist")
    live.add_argument("--output", type=Path, help="new private host directory; default: temporary directory")
    live.add_argument("--timeout", type=float, default=8, help="per-command timeout in seconds")
    offline = commands.add_parser("summarize", help="verify and summarize saved captures without ADB")
    offline.add_argument("report", type=Path)
    args = parser.parse_args()
    try:
        if args.operation == "collect":
            if not 0 < args.timeout <= 60:
                raise ValueError("--timeout must be greater than zero and at most 60 seconds")
            executable = args.adb or shutil.which("adb")
            local = ROOT / "out/host-tools/platform-tools/adb"
            if executable is None and local.is_file():
                executable = str(local)
            if executable is None:
                raise ValueError("ADB not found; install Android platform tools or pass --adb")
            output = collect(Adb(executable, args.timeout), args.output, args.serial)
            print(f"Read-only inspection saved to {output}")
            print("Native boot is pending; see hardware.md for the remaining evidence.")
        else:
            report = json.loads(args.report.read_text(), object_pairs_hook=unique_keys)
            print(markdown(summarize(report, args.report.parent)), end="")
    except (OSError, ValueError, KeyError, TypeError, subprocess.TimeoutExpired) as error:
        detail = "ADB command timed out" if isinstance(error, subprocess.TimeoutExpired) else str(error)
        if getattr(args, "serial", None):
            detail = redact(detail, args.serial)
        parser.exit(1, f"anne inspection: {detail}\n")


if __name__ == "__main__":
    main()
