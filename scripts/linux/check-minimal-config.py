#!/usr/bin/env python3
"""Enforce the Linux appliance minimal-configuration policy.

See docs/LINUX_APPLIANCE_MINIMAL.md. Given a *resolved* kernel .config (after
`olddefconfig`), assert that:

  * every entry on the baseline deny-list is off (=n / not set); and
  * every entry required by the chosen profile is on (=y).

Exits non-zero with a report on any violation. Used by the Linux appliance build.

Usage:
  scripts/linux/check-minimal-config.py --config out-dango/.config --flow gpu
"""
import argparse
import sys
from pathlib import Path

# Must be off in *every* appliance, regardless of flow. `PCI` is deliberately
# listed here and re-allowed only in flows that declare it.
DENY = [
    "MODULES", "BLOCK", "NET", "PCI", "USB", "SCSI", "ATA", "SOUND", "INPUT",
    "WIRELESS", "BT", "CGROUPS", "NAMESPACES", "AUDIT", "SECURITY",
    "BPF_SYSCALL", "FTRACE", "KPROBES", "SWAP", "HIBERNATION", "SUSPEND", "VT",
]

# Observed to resist `=n` on arm64 in the current baseline; reported as warnings
# rather than failures until their forcing path is removed. Evidence in
# docs/LINUX_APPLIANCE_MINIMAL.md ("Known forced-on").
KNOWN_FORCED = {
    "SMP": "arm64 Kconfig: `config SMP def_bool y` (architecture-forced)",
    "CPU_IDLE": "default y if ACPI; forcing path not yet removed",
    "PERF_EVENTS": "default y; forcing path not yet removed",
}

# Per-flow allow-lists: entries that must be ON for the flow to work, and the
# only deny-list entries the flow is permitted to turn on.
FLOWS = {
    "hello": {"require": [], "allow": []},
    "gpu": {
        "require": ["PCI", "PCI_HOST_GENERIC", "VIRTIO", "VIRTIO_PCI",
                    "DRM", "DRM_VIRTIO_GPU"],
        "allow": ["PCI"],
    },
    "amdgpu": {
        "require": ["PCI", "PCI_HOST_GENERIC", "VIRTIO", "VIRTIO_PCI",
                    "DRM", "DRM_AMDGPU"],
        "allow": ["PCI"],
    },
}


def parse_config(path: Path) -> dict:
    """CONFIG_FOO=y|m|value -> value; '# CONFIG_FOO is not set' -> 'n'."""
    values = {}
    for raw in path.read_text(errors="replace").splitlines():
        line = raw.strip()
        if line.startswith("# CONFIG_") and line.endswith(" is not set"):
            values[line[len("# CONFIG_"):-len(" is not set")]] = "n"
        elif line.startswith("CONFIG_") and "=" in line:
            key, _, val = line[len("CONFIG_"):].partition("=")
            values[key] = val.strip().strip('"')
    return values


def is_on(value: str) -> bool:
    return value in ("y", "m")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", type=Path, required=True)
    ap.add_argument("--flow", choices=sorted(FLOWS), required=True)
    args = ap.parse_args()

    if not args.config.is_file():
        print(f"error: no config at {args.config}", file=sys.stderr)
        return 2

    cfg = parse_config(args.config)
    flow = FLOWS[args.flow]
    problems = []

    for key in DENY:
        if key in flow["allow"]:
            continue
        if is_on(cfg.get(key, "n")):
            problems.append(f"  DENY  CONFIG_{key}={cfg[key]} (must be off for flow '{args.flow}')")

    if args.flow == "gpu":
        for key in ("DRM_NOUVEAU", "DRM_AMDGPU", "DRM_RADEON"):
            if is_on(cfg.get(key, "n")):
                problems.append(f"  DENY  CONFIG_{key} enabled in the virtio-only profile")

    warnings = []
    for key, why in KNOWN_FORCED.items():
        if is_on(cfg.get(key, "n")):
            warnings.append(f"  WARN  CONFIG_{key} is on ({why})")

    for key in flow["require"]:
        if not is_on(cfg.get(key, "n")):
            problems.append(f"  NEED  CONFIG_{key} is off (required by flow '{args.flow}')")

    if warnings:
        print(f"minimal-config warnings for flow '{args.flow}':")
        print("\n".join(warnings))

    if problems:
        print(f"minimal-config check FAILED for flow '{args.flow}' ({args.config}):")
        print("\n".join(problems))
        return 1

    print(f"minimal-config OK for flow '{args.flow}' ({args.config})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
