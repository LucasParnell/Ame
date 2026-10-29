#!/usr/bin/env bash
#
# Build the Anne native release kernel and package it for fastboot.
#
# Produces a directory with ame-native-kernel.img (flash this), the verified
# restore-stock-kernel.img, manifest.json, provenance.json and SHA256SUMS.
# Nothing is written to the phone.
#
# Usage:
#   scripts/anne/build-release.sh
#   scripts/anne/build-release.sh --output out/anne-release-test
#   scripts/anne/build-release.sh --no-build --output out/anne-release-test
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$ROOT/out/anne-usb-native-release"
OUTPUT=""
DO_BUILD=1
DO_PACKAGE=1
USE_PRESET=1

usage() {
    cat <<'EOF'
usage: build-release.sh [-o DIR] [-b DIR] [--no-build] [--no-package]

  -o, --output DIR   package directory (default: out/anne-release-<UTC stamp>)
  -b, --build-dir D  existing build tree (implies no preset)
      --no-build     skip the cmake build, reuse BUILD_DIR
      --no-package   build only
EOF
}

while (($#)); do
    case "$1" in
        -o|--output) OUTPUT="${2:?missing value for $1}"; shift 2 ;;
        -b|--build-dir) BUILD_DIR="${2:?missing value for $1}"; USE_PRESET=0; shift 2 ;;
        --no-build) DO_BUILD=0; shift ;;
        --no-package) DO_PACKAGE=0; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown argument: $1" >&2; usage; exit 2 ;;
    esac
done

# Homebrew is not always on a non-interactive PATH.
if ! command -v cmake >/dev/null 2>&1; then
    PATH="/opt/homebrew/bin:$PATH"
fi
command -v cmake >/dev/null 2>&1 || {
    echo "error: cmake not found; install it (brew install cmake) or add it to PATH" >&2
    exit 127
}
command -v ninja >/dev/null 2>&1 || {
    echo "error: ninja not found; install it (brew install ninja) or add it to PATH" >&2
    exit 127
}

if [[ -z "$OUTPUT" ]]; then
    OUTPUT="$ROOT/out/anne-release-$(date -u +%Y%m%d-%H%M%S)"
fi

if git -C "$ROOT" rev-parse --git-dir >/dev/null 2>&1; then
    changes="$(git -C "$ROOT" status --porcelain | wc -l | tr -d ' ')"
    if [[ "$changes" != "0" ]]; then
        echo "warning: $changes uncommitted path(s); provenance will record dirty=yes" >&2
    fi
fi

if ((DO_BUILD)); then
    if ((USE_PRESET)); then
        echo "== building anne-usb-native-release"
        cmake --build --preset anne-usb-native-release
    else
        echo "== building $BUILD_DIR"
        cmake --build "$BUILD_DIR"
    fi
fi

for artifact in kernel/anko.bin system.mxb manifest.json; do
    [[ -f "$BUILD_DIR/$artifact" ]] || {
        echo "error: missing $BUILD_DIR/$artifact; run without --no-build" >&2
        exit 1
    }
done

if ((DO_PACKAGE)); then
    PY="$ROOT/.venv/bin/python3"
    [[ -x "$PY" ]] || PY="$(command -v python3)"
    echo "== packaging $OUTPUT"
    "$PY" "$ROOT/scripts/anne/build-native-package.py" \
        --build "$BUILD_DIR" --output "$OUTPUT" >/dev/null
    (cd "$OUTPUT" && shasum -a 256 -c SHA256SUMS >/dev/null)
    "$PY" - "$OUTPUT/manifest.json" <<'PY'
import json, sys
manifest = json.load(open(sys.argv[1]))
print(f"model={manifest['model']} partition={manifest['partition']} "
      f"bytes={manifest['partition_bytes']}")
print(f"kernel={manifest['native_image_sha256']}")
print(f"restore={manifest['restore_kernel_sha256']}")
avb = manifest.get("avb", {})
print(f"avb_verified={avb.get('local_signature_and_hash_verified')} "
      f"bootloader_acceptance={avb.get('bootloader_acceptance')}")
PY
    echo "== package ready: $OUTPUT"
    echo "   flash:   scripts/anne/fastboot.py flash --package $OUTPUT --write"
    echo "   restore: scripts/anne/fastboot.py restore --package $OUTPUT --write"
fi
