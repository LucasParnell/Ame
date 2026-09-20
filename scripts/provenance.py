#!/usr/bin/env python3
"""Emit build provenance for an Ame build tree.

Writes two machine-readable artifacts from data the build already knows:

    <build-dir>/generated/provenance.json   # canonical, for tooling
    <build-dir>/generated/provenance.h      # C++ view, for embedding

The record captures the source revision (``git describe`` + dirty bit), the
configuration (build type, board, dev/release signing mode), the signing key
identity, and the sha256 of the produced ``system.mxb``.

Design rules:

* Data-oriented: every field is derived from a shell/dict lookup, no policy.
* Best-effort: a missing ``git``, an unreadable key, or an unsigned tree
  degrades that field to ``null``/``"unknown"`` and still writes both files.
  The script never exits non-zero for an environment problem, so it can sit in
  the ordinary build graph without being able to fail a build.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import subprocess
import sys
from pathlib import Path

SCHEMA = 1


def run_git(source_dir: Path, *args: str) -> tuple[int, str]:
    """Run git, returning (returncode, stdout). Missing git -> (-1, "")."""
    try:
        proc = subprocess.run(
            ["git", "-C", str(source_dir), *args],
            capture_output=True,
            text=True,
            timeout=10,
        )
    except (OSError, subprocess.SubprocessError):
        return -1, ""
    return proc.returncode, proc.stdout.strip()


def git_record(source_dir: Path) -> dict[str, object]:
    """Describe the source revision. All fields are null when git is absent."""
    code, describe = run_git(source_dir, "describe", "--tags", "--always", "--dirty")
    if code != 0 or not describe:
        # No git, or not a repository: leave the block explicit but empty.
        return {"describe": None, "commit": None, "branch": None, "dirty": None}

    dirty = describe.endswith("-dirty")
    _, commit = run_git(source_dir, "rev-parse", "HEAD")
    _, branch = run_git(source_dir, "rev-parse", "--abbrev-ref", "HEAD")
    return {
        "describe": describe,
        "commit": commit or None,
        "branch": branch or None,
        "dirty": dirty,
    }


def sha256_file(path: Path) -> str | None:
    if not path.is_file():
        return None
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_signing(source_dir: Path):
    """Import scripts/mx/signing.py, or return None when unavailable."""
    module_path = source_dir / "scripts" / "mx" / "signing.py"
    if not module_path.is_file():
        return None
    try:
        spec = importlib.util.spec_from_file_location("ame_signing", module_path)
        if spec is None or spec.loader is None:
            return None
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module
    except Exception:  # pragma: no cover - defensive: never fail the build
        return None


def signing_record(
    source_dir: Path,
    signing_key: str,
    trusted: list[str],
    build_dev: bool,
    trust_mode: str,
) -> dict[str, object]:
    """Identify the signing key by fingerprint and record the trust mode."""
    record: dict[str, object] = {
        "trust_mode": trust_mode,
        "key_id": "unknown",
        "signing_key_source": "development-seed" if build_dev else "file",
        "trusted_key_count": len([k for k in trusted if k]),
    }
    module = load_signing(source_dir)
    if module is None:
        return record
    try:
        signer = module.private_key(signing_key or None, development=build_dev)
        pub = module.public_bytes(signer.public_key())
        fingerprint = hashlib.sha256(pub).hexdigest()[:16]
        prefix = "dev" if build_dev else "release"
        record["key_id"] = f"{prefix}-{fingerprint}"
    except Exception:
        # A missing key already fails the configure/build elsewhere; provenance
        # only reports what it can see.
        record["key_id"] = "unknown"
    return record


def as_bool(value: str) -> bool:
    return value.strip().lower() in {"1", "on", "true", "yes"}


def build_document(args: argparse.Namespace) -> dict[str, object]:
    source_dir = Path(args.source_dir).resolve()
    system_mxb = Path(args.system_mxb).resolve()
    build_dev = as_bool(args.build_dev)

    signing = signing_record(
        source_dir,
        args.signing_key,
        args.trusted or [],
        build_dev,
        args.trust_mode,
    )
    return {
        "schema": SCHEMA,
        "generator": "scripts/provenance.py",
        "git": git_record(source_dir),
        "build": {
            "type": args.build_type,
            "board": args.board,
            "build_dev": build_dev,
            "mode": "dev" if build_dev else "release",
        },
        "signing": signing,
        "artifact": {
            "name": "system.mxb",
            "path": str(system_mxb),
            "sha256": sha256_file(system_mxb),
        },
    }


def render_header(document: dict[str, object]) -> str:
    """Render a small C++ view of the document as constexpr byte arrays."""

    def literal(value: object) -> str:
        return json.dumps("null" if value is None else str(value))

    git = document["git"]  # type: ignore[index]
    build = document["build"]  # type: ignore[index]
    signing = document["signing"]  # type: ignore[index]
    artifact = document["artifact"]  # type: ignore[index]
    dirty = git["dirty"]  # type: ignore[index]
    lines = [
        "// Generated by scripts/provenance.py. Do not edit.",
        "#pragma once",
        "",
        "namespace ame::provenance {",
        f"inline constexpr int kSchema = {document['schema']};",
        f"inline constexpr char kGitDescribe[] = {literal(git['describe'])};",
        f"inline constexpr char kGitCommit[] = {literal(git['commit'])};",
        f"inline constexpr char kGitBranch[] = {literal(git['branch'])};",
        f"inline constexpr int kGitDirty = {1 if dirty else 0 if dirty is not None else -1};",
        f"inline constexpr char kBuildType[] = {literal(build['type'])};",
        f"inline constexpr char kBoard[] = {literal(build['board'])};",
        f"inline constexpr bool kBuildDev = {'true' if build['build_dev'] else 'false'};",
        f"inline constexpr char kTrustMode[] = {literal(signing['trust_mode'])};",
        f"inline constexpr char kSigningKeyId[] = {literal(signing['key_id'])};",
        f"inline constexpr char kSystemMxbSha256[] = {literal(artifact['sha256'])};",
        "}  // namespace ame::provenance",
        "",
    ]
    return "\n".join(lines)


def write_if_changed(path: Path, text: str) -> None:
    """Write text, leaving mtime untouched when content is identical.

    Keeping the file stable across no-op builds preserves Ninja's ability to
    report "no work to do".
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and path.read_text(encoding="utf-8") == text:
        return
    path.write_text(text, encoding="utf-8")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Emit Ame build provenance.")
    parser.add_argument("--source-dir", required=True)
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--output-json", required=True)
    parser.add_argument("--output-header", required=True)
    parser.add_argument("--build-type", default="unknown")
    parser.add_argument("--board", default="unknown")
    parser.add_argument("--build-dev", default="0")
    parser.add_argument("--trust-mode", default="development")
    parser.add_argument("--system-mxb", required=True)
    parser.add_argument("--signing-key", default="")
    parser.add_argument("--trusted", nargs="*", default=[])
    args = parser.parse_args(argv)

    document = build_document(args)
    write_if_changed(
        Path(args.output_json),
        json.dumps(document, indent=2, sort_keys=True) + "\n",
    )
    write_if_changed(Path(args.output_header), render_header(document))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
