#!/usr/bin/env bash
# Install the built Ame system onto the QEMU virtual filesystem image.
#
# The installed tree deliberately keeps the kernel and userland artifacts in
# separate directories:
#   /ame/kernel/anko
#   /ame/boot/system.mxb
#   /ame/userland/manifest.json
#   /ame/userland/manifest.v0
#   /ame/userland/bin/*.mxi
#
# Environment:
#   AME_BUILD_DIR       Build directory. Defaults to ./build.
#   AME_FS_DISK         Destination disk image. Defaults to $AME_BUILD_DIR/fs.img.
#   AME_FS_DISK_SIZE    Size used when formatting/creating the image. Defaults to 128M.
#   AME_VFS_INCLUDE_INITRD=1 also copies build/initrd.mxb to /ame/boot/initrd.mxb.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${AME_BUILD_DIR:-${ROOT}/build}"
FS_DISK="${AME_FS_DISK:-${BUILD_DIR}/fs.img}"
FS_DISK_SIZE="${AME_FS_DISK_SIZE:-128M}"
INCLUDE_INITRD="${AME_VFS_INCLUDE_INITRD:-0}"

require_file() {
    if [[ ! -f "$1" ]]; then
        echo "error: missing build artifact: $1" >&2
        echo "hint: run cmake --build ${BUILD_DIR}" >&2
        exit 1
    fi
}

KERNEL="${BUILD_DIR}/kernel/anko"
USERLAND_DIR="${BUILD_DIR}/userland"
USERLAND_MANIFEST="${USERLAND_DIR}/manifest.v0"
if [[ ! -f "${USERLAND_MANIFEST}" ]]; then
    USERLAND_MANIFEST="${ROOT}/userland/manifest.v0"
fi
SYSTEM_MANIFEST="${BUILD_DIR}/manifest.json"
SYSTEM_BUNDLE="${BUILD_DIR}/system.mxb"
INITRD="${BUILD_DIR}/initrd.mxb"

require_file "${KERNEL}"
require_file "${USERLAND_MANIFEST}"
require_file "${SYSTEM_MANIFEST}"
require_file "${SYSTEM_BUNDLE}"
shopt -s nullglob
USERLAND_IMAGES=("${USERLAND_DIR}"/*.mxi)
shopt -u nullglob
if (( ${#USERLAND_IMAGES[@]} == 0 )); then
    echo "error: no userland MXI artifacts found in ${USERLAND_DIR}" >&2
    echo "hint: run cmake --build ${BUILD_DIR} --target mochi_initrd" >&2
    exit 1
fi
if [[ "${INCLUDE_INITRD}" == "1" ]]; then
    require_file "${INITRD}"
fi

mkdir -p "$(dirname "${FS_DISK}")"

ARGS=(
    create
    --output "${FS_DISK}"
    --build-dir "${BUILD_DIR}"
    --size "${FS_DISK_SIZE}"
)
if [[ "${INCLUDE_INITRD}" == "1" ]]; then
    ARGS+=(--include-initrd)
fi

python3 "${ROOT}/scripts/mx/mkamefs.py" "${ARGS[@]}"
