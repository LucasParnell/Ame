#!/usr/bin/env bash
set -euo pipefail

AME="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${DANGO_LINUX_OUT:-${AME}/build/dango-linux}"
mkdir -p "${OUT}"

CFLAGS=(
  -nostdlib
  -static
  -ffreestanding
  -fno-stack-protector
  -fno-builtin
  -Os
  -Wl,-e,_start
)

if [[ "${DANGO_LINUX_PROFILE:-hello}" == gpu ]]; then
    CFLAGS+=(-DDANGO_GPU_PROBE=1)
fi

SRC="${AME}/linux-artifacts/agent/dango-agent.c"
DST="${OUT}/dango-agent"

if [[ -n "${CROSS_COMPILE:-}" ]] && command -v "${CROSS_COMPILE}gcc" >/dev/null 2>&1; then
    "${CROSS_COMPILE}gcc" "${CFLAGS[@]}" -o "${DST}" "${SRC}"
elif command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
    aarch64-linux-gnu-gcc "${CFLAGS[@]}" -o "${DST}" "${SRC}"
elif command -v gcc >/dev/null 2>&1 && [[ "$(uname -s)" == "Linux" && "$(uname -m)" == "aarch64" ]]; then
    # Native aarch64 Linux host (e.g. the build container): plain gcc targets
    # aarch64-linux directly.
    gcc "${CFLAGS[@]}" -o "${DST}" "${SRC}"
elif command -v clang >/dev/null 2>&1; then
    clang --target=aarch64-linux-gnu -fuse-ld=lld "${CFLAGS[@]}" -o "${DST}" "${SRC}"
else
    echo "error: no AArch64 Linux compiler found" >&2
    echo "Set CROSS_COMPILE=aarch64-linux-gnu- or install clang/lld." >&2
    exit 1
fi

file "${DST}" | grep -q 'ARM aarch64' || {
    echo "error: ${DST} is not an AArch64 ELF" >&2
    file "${DST}" >&2 || true
    exit 1
}
