#!/usr/bin/env python3
"""Build the signed Ame system bundle and JSON boot manifest."""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def build_manifest(build_dir: Path, descriptor: Path, profile: str) -> dict[str, object]:
    from system_descriptor import load_profile

    manifest = load_profile(descriptor, profile)
    userland_dir = build_dir / "userland"
    init_image = userland_dir / "init.mxi"
    for svc in manifest["services"]:
        svc["sha256"] = sha256_file(userland_dir / f"{svc['object']}.mxi")
    manifest["boot"] = {"init": {
        "bundle": "/ame/boot/system.mxb", "object": "init",
        "sha256": sha256_file(init_image),
    }}
    manifest["bundles"] = [{
        "id": "system", "path": "/ame/boot/system.mxb", "trust_key": "dev-ed25519",
    }]
    return manifest


def legacy_view(manifest: dict) -> str:
    """Generated compatibility view for tools that still display the V0 list."""
    lines = ["# Generated from system.json; edit the descriptor, not this file."]
    for service in manifest["services"]:
        fields = [str(service[key]) for key in ("name", "object", "principal", "restart")]
        fields.append(",".join(service["caps"]) or "-")
        lines.append(" ".join(fields))
    return "\n".join(lines) + "\n"


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--descriptor", type=Path, required=True)
    parser.add_argument("--profile", required=True)
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
    manifest = build_manifest(build_dir, args.descriptor.resolve(), args.profile)
    args.manifest_output.parent.mkdir(parents=True, exist_ok=True)
    args.manifest_output.write_text(
        json.dumps(manifest, sort_keys=True, separators=(",", ":")) + "\n",
        encoding="utf-8",
    )

    userland_dir = build_dir / "userland"
    (userland_dir / "manifest.v0").write_text(legacy_view(manifest), encoding="utf-8")
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
