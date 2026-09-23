#!/usr/bin/env python3
"""check-caps.py — validate a generated Ame boot manifest.

This is the tooling half of the capability story. The manifest is the source of
*authority*: a service is granted exactly the caps it declares. This checker
proves the declared grants are internally consistent — every dependency and cap
resolves, no name is declared twice, principals and restart policies are known.

It deliberately does NOT try to infer what a service's code needs; that is the
job of the still-to-come requirement pass (images carry no MxImageRequirement
records yet). The rule from the design discussion holds: the compiler may
*verify* requirements against the manifest, but it must never *grant* — so this
checker treats the manifest as authoritative and checks it, nothing more.

Usage:
    check-caps.py [--manifest out/qemu-dev/manifest.json]

Exit 0 when consistent, 1 with reasons otherwise, 2 on a missing manifest.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

PRINCIPALS = {"kernel", "native_trusted", "native_sandboxed", "vm"}
RESTARTS = {"always", "never"}


def check_manifest(manifest: dict) -> list[str]:
    problems: list[str] = []
    services = manifest.get("services") or []
    resources = manifest.get("resources") or []
    channels = manifest.get("channels") or []

    resource_names = [r.get("name") for r in resources]
    if len(resource_names) != len(set(resource_names)):
        problems.append("resources: a resource name is declared more than once")
    resource_set = set(resource_names)

    channel_names = [c.get("name") for c in channels]
    if len(channel_names) != len(set(channel_names)):
        problems.append("channels: a channel name is declared more than once")

    names = [s.get("name") for s in services]
    if len(names) != len(set(names)):
        problems.append("services: a service name is declared more than once")
    name_set = set(names)

    # `uses` names a *provided* service, not a process: a client resolves "fs" or
    # "nameservice", and whichever process provides it is an implementation
    # detail. Gather the provide table first, then validate against it.
    provided: dict[str, str] = {}
    for svc in services:
        for prov in svc.get("provides") or []:
            if prov in provided:
                problems.append(
                    f"service {svc.get('name')}: provides {prov!r} already provided by {provided[prov]}"
                )
            else:
                provided[prov] = svc.get("name")

    for svc in services:
        name = svc.get("name")
        if svc.get("principal") not in PRINCIPALS:
            problems.append(f"service {name}: unknown principal {svc.get('principal')!r}")
        if svc.get("restart") not in RESTARTS:
            problems.append(f"service {name}: unknown restart policy {svc.get('restart')!r}")
        if not svc.get("object"):
            problems.append(f"service {name}: missing object name")
        for used in svc.get("uses") or []:
            if used not in provided:
                problems.append(f"service {name}: uses {used!r}, which nothing provides")
        for dep in svc.get("start_after") or []:
            if dep not in name_set:
                problems.append(f"service {name}: start_after unknown service {dep!r}")
        for cap in svc.get("caps") or []:
            if cap not in resource_set:
                problems.append(f"service {name}: cap {cap!r} is not a declared resource")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, default=Path("out/qemu-dev/manifest.json"))
    args = parser.parse_args()

    if not args.manifest.exists():
        print(f"check-caps: manifest not found: {args.manifest}", file=sys.stderr)
        return 2

    manifest = json.loads(args.manifest.read_text())
    problems = check_manifest(manifest)
    if problems:
        print("check-caps: manifest is inconsistent:")
        for problem in problems:
            print(f"  - {problem}")
        return 1

    services = manifest.get("services") or []
    print(
        "check-caps: ok "
        f"({len(services)} services, {len(manifest.get('resources') or [])} resources, "
        f"{len(manifest.get('channels') or [])} channels)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
