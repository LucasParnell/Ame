#!/usr/bin/env bash
# scripts/build-switch-payload.sh — compatibility wrapper for the old Switch
# package command.  Ame is now staged as an L4T/U-Boot ARM64 Image, not as a
# direct Hekate `payload=` RCM payload.
#
# Usage:
#   scripts/build-switch-payload.sh [BUILD_DIR] [SD_MOUNT]
#
#   BUILD_DIR  CMake build directory (default: build-switch)
#   SD_MOUNT   Path to mounted SD card root (default: /mnt/sdcard)
#              If not given, only the build/staging steps run.
#
# Prerequisites:
#   cmake -S . -B build-switch -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=scripts/build/toolchain-aarch64.cmake \
#         -DANKO_BOARD=switch-t210 -DANKO_BUILD_DEV=ON
#
# SD card layout after install is produced by build-hekate-package.sh.

set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${1:-${REPO}/build-switch}"
SD_MOUNT="${2:-}"

args=(--build-dir "${BUILD_DIR}")
if [[ -n "${SD_MOUNT}" ]]; then
    args+=("${SD_MOUNT}")
fi

exec "${REPO}/scripts/build-hekate-package.sh" "${args[@]}"
