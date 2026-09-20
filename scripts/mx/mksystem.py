#!/usr/bin/env python3
"""Build the signed Ame system bundle and JSON boot manifest."""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path


RESOURCE_ALIASES = [
    {"name": "uart", "boot": "uart.mmio"},
    {"name": "vm", "boot": "vm.root"},
    {"name": "mmio", "boot": "device.mmio"},
    {"name": "blk", "boot": "virtio.blk"},
    {"name": "shell", "boot": "terminal"},
]

CHANNEL_ALIASES = [{'name': 'init_log_ring', 'kind': 'ring', 'order': 6}, {'name': 'console_mirror_ring', 'kind': 'ring', 'order': 10}, {'name': 'console_mirror_db', 'kind': 'doorbell'}, {'name': 'ring_ping_to_pong', 'kind': 'ring', 'order': 6}, {'name': 'ring_pong_to_ping', 'kind': 'ring', 'order': 6}, {'name': 'db_pong', 'kind': 'doorbell'}, {'name': 'db_ping', 'kind': 'doorbell'}, {'name': 'db_ipc_done', 'kind': 'doorbell'}, {'name': 'ns_request_ring', 'kind': 'ring', 'order': 6}, {'name': 'ns_reply_ring', 'kind': 'ring', 'order': 6}, {'name': 'db_nameservice', 'kind': 'doorbell'}, {'name': 'db_ns_reply', 'kind': 'doorbell'}, {'name': 'db_dango_done', 'kind': 'doorbell'}, {'name': 'db_dango_auth_gate', 'kind': 'doorbell'}, {'name': 'db_dango_layout_done', 'kind': 'doorbell'}, {'name': 'fb_cmd_ring', 'kind': 'ring', 'order': 10}, {'name': 'fb_reply_ring', 'kind': 'ring', 'order': 6}, {'name': 'fb_cmd_db', 'kind': 'doorbell'}, {'name': 'fb_reply_db', 'kind': 'doorbell'}, {'name': 'xhci_hid_ring', 'kind': 'ring', 'order': 6}, {'name': 'xhci_hid_db', 'kind': 'doorbell'}, {'name': 'hid_input_ring', 'kind': 'ring', 'order': 6}, {'name': 'hid_input_db', 'kind': 'doorbell'}, {'name': 'input_tty_ring', 'kind': 'ring', 'order': 6}, {'name': 'input_tty_db', 'kind': 'doorbell'}, {'name': 'blk_req_ring', 'kind': 'ring', 'order': 6}, {'name': 'blk_reply_ring', 'kind': 'ring', 'order': 6}, {'name': 'blk_req_db', 'kind': 'doorbell'}, {'name': 'blk_reply_db', 'kind': 'doorbell'}, {'name': 'fs_cmd_ring', 'kind': 'ring', 'order': 6}, {'name': 'fs_reply_ring', 'kind': 'ring', 'order': 6}, {'name': 'fs_cmd_db', 'kind': 'doorbell'}, {'name': 'fs_reply_db', 'kind': 'doorbell'}, {'name': 'term_input_ring', 'kind': 'ring', 'order': 6}, {'name': 'term_output_ring', 'kind': 'ring', 'order': 6}, {'name': 'term_input_db', 'kind': 'doorbell'}, {'name': 'term_output_db', 'kind': 'doorbell'}, {'name': 'svc_ctl_req_ring', 'kind': 'ring', 'order': 6}, {'name': 'svc_ctl_reply_ring', 'kind': 'ring', 'order': 6}, {'name': 'svc_ctl_req_db', 'kind': 'doorbell'}, {'name': 'svc_ctl_reply_db', 'kind': 'doorbell'}]

CHECKS = [
    {"name": "ipc", "service": "demo.ping"},
    {"name": "blk", "service": "drv.blk.virtio"},
    {"name": "fs", "service": "io.fs"},
]


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def parse_legacy_manifest(path: Path) -> list[dict[str, object]]:
    services: list[dict[str, object]] = []
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) != 5:
            raise SystemExit(f"{path}: bad manifest line: {raw!r}")
        name, image, principal, restart, caps = parts
        cap_list = [] if caps == "-" else caps.split(",")
        services.append(
            {
                "name": name,
                "object": image,
                "bundle": "system",
                "principal": principal,
                "restart": restart,
                "stage": stage_for(name),
                "start_after": dependencies_for(name),
                "caps": cap_list,
                "affinity": affinity_for(name),
                "provides": provides_for(name),
                "uses": (["fs", "nameservice"] if name == "app.hello" else ["fs"] if name == "msh" else ["nameservice"] if name == "demo.ping" else []),
            }
        )
    return services


def stage_for(name: str) -> str:
    if name in {"io.console", "nameservice", "demo.pong", "demo.ping"}:
        return "base"
    if name in {"drv.blk.virtio", "io.fs", "msh"}:
        return "storage"
    if name.startswith("drv.") or name.startswith("io.") or name.startswith("tty.") or name == "ui.input":
        return "drivers"
    if name.startswith("dango."):
        return "vm"
    return "late"


def dependencies_for(name: str) -> list[str]:
    deps: dict[str, list[str]] = {
        "nameservice": ["io.console"],
        "demo.pong": ["io.console"],
        "demo.ping": ["nameservice", "demo.pong"],
        "dango.manager": ["io.console"],
        "dango.auth_probe": ["dango.manager"],
        "tty.fb0": ["io.framebuffer"],
        "drv.usb.hid": ["drv.usb.xhci"],
        "ui.input": ["drv.usb.hid"],
        "drv.blk.virtio": ["io.console"],
        "io.fs": ["drv.blk.virtio"],
        "msh": ["io.fs", "tty.fb0"],
        "app.hello": ["nameservice", "io.fs"],
    }
    return deps.get(name, ["io.console"] if name != "io.console" else [])


def affinity_for(name: str) -> int:
    if name == "demo.pong":
        return 1
    return 0


def provides_for(name: str) -> list[str]:
    if name == "nameservice":
        return ["nameservice"]
    if name == "io.fs":
        return ["fs"]
    if name == "drv.blk.virtio":
        return ["blk"]
    if name == "io.console":
        return ["console"]
    if name == "msh":
        return ["shell"]
    return []


def build_manifest(build_dir: Path, legacy_manifest: Path) -> dict[str, object]:
    userland_dir = build_dir / "userland"
    init_image = userland_dir / "init.mxi"
    if not init_image.is_file():
        raise FileNotFoundError(init_image)

    services = parse_legacy_manifest(legacy_manifest)
    for svc in services:
        image = userland_dir / f"{svc['object']}.mxi"
        if not image.is_file():
            raise FileNotFoundError(image)
        svc["sha256"] = sha256_file(image)

    return {
        "schema": 2,
        "boot": {
            "init": {
                "bundle": "/ame/boot/system.mxb",
                "object": "init",
                "sha256": sha256_file(init_image),
            }
        },
        "bundles": [
            {
                "id": "system",
                "path": "/ame/boot/system.mxb",
                "trust_key": "dev-ed25519",
            }
        ],
        "services": services,
        "resources": RESOURCE_ALIASES,
        "channels": CHANNEL_ALIASES,
        "checks": CHECKS,
    }


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--legacy-manifest", type=Path, required=True)
    parser.add_argument("--manifest-output", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--object",
        action="append",
        default=[],
        help="Additional mxpack object spec (kind:name:path), for packaged resources such as linux/Image",
    )
    args = parser.parse_args(argv)

    build_dir = args.build_dir.resolve()
    manifest = build_manifest(build_dir, args.legacy_manifest.resolve())
    args.manifest_output.parent.mkdir(parents=True, exist_ok=True)
    args.manifest_output.write_text(
        json.dumps(manifest, sort_keys=True, separators=(",", ":")) + "\n",
        encoding="utf-8",
    )

    userland_dir = build_dir / "userland"
    objects = ["--object", f"exec:init:{userland_dir / 'init.mxi'}"]
    for svc in manifest["services"]:  # type: ignore[index]
        obj = svc["object"]  # type: ignore[index]
        objects.extend(["--object", f"exec:{obj}:{userland_dir / (str(obj) + '.mxi')}"])
    for extra_object in args.object:
        objects.extend(["--object", extra_object])

    mxpack = Path(__file__).with_name("mxpack.py")
    cmd = [
        sys.executable,
        str(mxpack),
        "create",
        "--output",
        str(args.output),
        "--manifest",
        str(args.manifest_output),
        "--manifest-name",
        "manifest.json",
        *objects,
    ]
    subprocess.run(cmd, check=True)
    print(f"packed Ame system bundle to {args.output}")
    print(f"wrote JSON boot manifest to {args.manifest_output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
