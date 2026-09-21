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
  -Wall -Wextra -Werror
  -I"${AME}/shared/include"
  -Wl,-e,_start
)

SRCS=("${AME}/linux-artifacts/agent/dango-agent.c")

if [[ "${DANGO_LINUX_PROFILE:-hello}" == gpu ]]; then
    CFLAGS+=(-DDANGO_GPU_PROBE=1)
    # Phase F: the KMS self-test is a separate freestanding translation unit
    # (linux-artifacts/kms_test/) linked into the appliance init binary.
    CFLAGS+=(-DDANGO_GPU_KMS=1 -DDANGO_CHANNEL_TEST=1)
    SRCS+=("${AME}/linux-artifacts/channel_test/channel_test.c")
    SRCS+=("${AME}/linux-artifacts/kms_test/kms_test.c")
fi

DST="${OUT}/dango-agent"

if [[ -n "${CROSS_COMPILE:-}" ]] && command -v "${CROSS_COMPILE}gcc" >/dev/null 2>&1; then
    "${CROSS_COMPILE}gcc" "${CFLAGS[@]}" -o "${DST}" "${SRCS[@]}"
elif command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
    aarch64-linux-gnu-gcc "${CFLAGS[@]}" -o "${DST}" "${SRCS[@]}"
elif command -v gcc >/dev/null 2>&1 && [[ "$(uname -s)" == "Linux" && "$(uname -m)" == "aarch64" ]]; then
    # Native aarch64 Linux host (e.g. the build container): plain gcc targets
    # aarch64-linux directly.
    gcc "${CFLAGS[@]}" -o "${DST}" "${SRCS[@]}"
elif command -v clang >/dev/null 2>&1; then
    clang --target=aarch64-linux-gnu -fuse-ld=lld "${CFLAGS[@]}" -o "${DST}" "${SRCS[@]}"
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
