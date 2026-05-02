#!/usr/bin/env bash
set -euo pipefail

AME="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:-${AME}/build/dango-linux}"

required=(Image initramfs.cpio dango-linux.dtb dango-agent manifest.toml)
for name in "${required[@]}"; do
    if [[ ! -s "${OUT}/${name}" ]]; then
        echo "error: missing or empty ${OUT}/${name}" >&2
        exit 1
    fi
done

file "${OUT}/dango-agent" | grep -q 'ARM aarch64' || {
    echo "error: dango-agent is not an AArch64 ELF" >&2
    file "${OUT}/dango-agent" >&2 || true
    exit 1
}

python3 "${AME}/scripts/linux/check-initramfs.py" "${OUT}/initramfs.cpio"

# Validate that the DTB at least has the expected magic bytes.
python3 - "${OUT}/dango-linux.dtb" <<'PY'
import struct, sys
path = sys.argv[1]
with open(path, 'rb') as f:
    data = f.read(4)
if len(data) != 4 or struct.unpack('>I', data)[0] != 0xd00dfeed:
    raise SystemExit(f"error: {path} is not a valid DTB blob")
PY

echo "DANGO LINUX ARTIFACTS CHECK OK"
ls -lh "${OUT}/Image" "${OUT}/initramfs.cpio" "${OUT}/dango-linux.dtb" "${OUT}/dango-agent" "${OUT}/manifest.toml"
