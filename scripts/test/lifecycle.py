#!/usr/bin/env python3
"""Appliance-lifecycle host regression (Phase K).

The kernel lifecycle matrix (lifecycle.mxb) runs on target and covers the
channel boot-generation, ring back-pressure and teardown-ordering invariants.
This host row covers the part of Phase K that needs no VM/guest: the IOMMU
domain-quarantine lifecycle an appliance death relies on, compiled straight from
kernel/iommu/core.cc (the same way tests/test_iommu.py does).

Prints a single JSON object on success; the verify gate parses `success`.
"""
import json
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def build_and_run(work):
    probe = str(Path(work) / "probe")
    build = subprocess.run(
        [
            "clang++", "-std=c++23", "-DANKO_HOST_TEST", "-O1", "-g",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-Wall", "-Wextra", "-Werror",
            "-I" + str(ROOT / "kernel/include"),
            "-I" + str(ROOT / "shared/include"),
            "-idirafter", str(ROOT / "lib/include"),
            str(ROOT / "tests/kernel/iommu_lifecycle_probe.cc"),
            str(ROOT / "kernel/iommu/core.cc"),
            "-o", probe,
        ],
        capture_output=True, text=True,
    )
    if build.returncode != 0:
        return False, "build failed", build.stderr.strip()[-400:]
    run = subprocess.run([probe], capture_output=True, text=True, timeout=30)
    if run.returncode != 0:
        return False, "probe failed", (run.stderr or run.stdout).strip()[-400:]
    if "iommu lifecycle probe ok" not in run.stdout:
        return False, "marker missing", run.stdout.strip()[-400:]
    return True, "iommu lifecycle probe ok", ""


def main():
    start = time.time()
    with tempfile.TemporaryDirectory(prefix="ame-lifecycle-") as work:
        ok, reason, detail = build_and_run(work)
    result = {"success": ok, "reason": reason, "elapsed": round(time.time() - start, 3)}
    if detail:
        result["detail"] = detail
    print(json.dumps(result))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
