#!/usr/bin/env python3
"""Package the durability persistence fixture (app.durable) with the whole Ame
system into `durable.mxb`.

The default `system.mxb` is left untouched: the fixture service is added only to
this bundle's manifest, so a normal boot never runs it. The bundle is a complete
system (init + every service) plus the extra probe, so
`boot.py --bundle durable.mxb` boots the ordinary userland with the probe wired.

Usage: package-durable.py <build-dir>
"""
from pathlib import Path
import hashlib
import json
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "mx"))
import mxpack  # noqa: E402


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        raise SystemExit(__doc__)
    build = Path(argv[1]).resolve()
    userland = build / "userland"
    manifest = json.loads((build / "manifest.json").read_text())

    probe_mxi = userland / "app.durable.mxi"
    if not probe_mxi.is_file():
        raise SystemExit(f"missing {probe_mxi} (build the durable_bundle target)")

    probe = {
        "name": "app.durable",
        "object": "app.durable",
        "bundle": "system",
        "principal": "native_sandboxed",
        "restart": "never",
        "stage": "late",
        "start_after": ["io.fs"],
        "caps": [],
        "affinity": 0,
        "provides": [],
        "uses": ["fs"],
        "sha256": sha256_file(probe_mxi),
    }
    if any(s.get("name") == "app.durable" for s in manifest["services"]):
        raise SystemExit("manifest already contains app.durable")
    manifest["services"].append(probe)

    manifest_path = build / "durable-manifest.json"
    manifest_path.write_text(json.dumps(manifest, sort_keys=True, separators=(",", ":")) + "\n")

    objects = ["exec:init:" + str(userland / "init.mxi")]
    for svc in manifest["services"]:
        obj = str(svc["object"])
        objects.append(f"exec:{obj}:" + str(userland / f"{obj}.mxi"))

    mxpack.build_bundle(build / "durable.mxb", manifest_path, objects)
    print(f"packed durability fixture bundle to {build / 'durable.mxb'}")
    print(f"wrote fixture manifest to {manifest_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
