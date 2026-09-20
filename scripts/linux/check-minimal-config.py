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
    "BPF_SYSCALL", "PERF_EVENTS", "FTRACE", "KPROBES", "SWAP", "HIBERNATION",
    "SUSPEND", "SMP", "CPU_IDLE", "VT",
]

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

    for key in flow["require"]:
        if not is_on(cfg.get(key, "n")):
            problems.append(f"  NEED  CONFIG_{key} is off (required by flow '{args.flow}')")

    if problems:
        print(f"minimal-config check FAILED for flow '{args.flow}' ({args.config}):")
        print("\n".join(problems))
        return 1

    print(f"minimal-config OK for flow '{args.flow}' ({args.config})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
