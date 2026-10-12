#!/usr/bin/env python3
"""Read Huawei fastboot identity and lock information without changing the phone."""

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
# Huawei may reject generic product/unlocked variables. These OEM commands are
# information queries; no flash, erase, unlock, reboot, boot, or download occurs.
QUERIES = {
    "bootloader_status": ("oem", "get-bootinfo"),
    "model": ("oem", "get-product-model"),
    "firmware_build": ("oem", "get-build-number"),
    "max_download_size": ("getvar", "max-download-size"),
}


def query_result(result, serial):
    response = (result.stdout + result.stderr).decode("utf-8", errors="replace")
    response = response.replace(serial, "<device-serial>").strip()
    # Some fastboot versions exit successfully even when the device sent FAIL.
    failed = result.returncode != 0 or bool(re.search(r"\bFAILED\b|Command not allowed", response))
    return {"status": "unsupported-or-failed" if failed else "ok",
            "returncode": result.returncode, "response": response[:16384]}


def successful_response(report, name):
    entry = report["queries"].get(name, {})
    return entry.get("response", "") if entry.get("status") == "ok" else ""


def summarize(report):
    model_response = successful_response(report, "model")
    models = set(re.findall(r"\bANE-[A-Z0-9]+\b", model_response, re.IGNORECASE))
    model = next(iter(models)).upper() if len(models) == 1 else None
    lock_response = successful_response(report, "bootloader_status")
    states = set(re.findall(r"\b(?:unlocked|locked)\b", lock_response.lower()))
    unlocked = True if states == {"unlocked"} else False if states == {"locked"} else None
    return {"model": model, "target_confirmed": model is not None,
            "bootloader_unlocked": unlocked, "twrp_installed": None,
            "native_boot_ready": False,
            "remaining": ["confirm installed recovery and obtain a recovery shell",
                          "capture kernel configuration, DTB and physical hardware layout",
                          "verify a native handoff and return to recovery"]}


def inspect(executable, output=None, serial=None, timeout=6, run=subprocess.run):
    devices = run([executable, "devices"], capture_output=True, timeout=timeout, check=False)
    if devices.returncode:
        raise ValueError("fastboot device enumeration failed")
    targets = [line.split() for line in devices.stdout.decode().splitlines() if line.split()]
    if serial is not None:
        targets = [row for row in targets if row[0] == serial]
    if len(targets) != 1 or len(targets[0]) < 2 or targets[0][1] != "fastboot":
        raise ValueError("expected one fastboot transport; connect the phone or specify --serial")
    serial = targets[0][0]
    if output is None:
        output = Path(tempfile.mkdtemp(prefix="ame-anne-fastboot-"))
    else:
        output = output.expanduser().resolve()
        output.mkdir(mode=0o700, parents=True, exist_ok=False)
    report = {"schema": 1, "operation": "read-only-fastboot",
              "collected_utc": datetime.now(timezone.utc).isoformat(),
              "device_id": hashlib.sha256(serial.encode()).hexdigest()[:12], "queries": {}}
    for name, arguments in QUERIES.items():
        try:
            result = run([executable, "-s", serial, *arguments], capture_output=True,
                         timeout=timeout, check=False)
            entry = query_result(result, serial)
        except subprocess.TimeoutExpired:
            entry = {"status": "timeout", "response": ""}
        report["queries"][name] = entry
        # Once transport responses stall, do not enqueue more device commands.
        if entry["status"] == "timeout":
            break
    report["summary"] = summarize(report)
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return output, report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fastboot", help="fastboot executable; default PATH or out/host-tools")
    parser.add_argument("--serial", help="select one fastboot device")
    parser.add_argument("--output", type=Path, help="new private host directory; default temporary")
    parser.add_argument("--timeout", type=float, default=6, help="per-query timeout in seconds")
    args = parser.parse_args()
    try:
        if not 0 < args.timeout <= 60:
            raise ValueError("--timeout must be greater than zero and at most 60 seconds")
        executable = args.fastboot or shutil.which("fastboot")
        local = ROOT / "out/host-tools/platform-tools/fastboot"
        if executable is None and local.is_file():
            executable = str(local)
        if executable is None:
            raise ValueError("fastboot not found; install platform tools or pass --fastboot")
        output, report = inspect(executable, args.output, args.serial, args.timeout)
        print(f"Read-only fastboot report saved to {output}")
        print(json.dumps(report["summary"], indent=2))
        if any(query["status"] == "timeout" for query in report["queries"].values()):
            parser.exit(1, "Fastboot query timed out; collection stopped without changing the phone.\n")
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        detail = "fastboot enumeration timed out" if isinstance(error, subprocess.TimeoutExpired) else str(error)
        if args.serial:
            detail = detail.replace(args.serial, "<device-serial>")
        parser.exit(1, f"anne fastboot inspection: {detail}\n")


if __name__ == "__main__":
    main()
