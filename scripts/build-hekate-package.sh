#!/usr/bin/env bash
# scripts/build-hekate-package.sh — Build Ame for Nintendo Switch and package for Hekate.
#
# Usage:
#   scripts/build-hekate-package.sh [options] [SD_MOUNT]
#
#   SD_MOUNT    Path to mounted SD card root (optional).
#               If not given, only build + staging are performed.
#
# Options:
#   --build-dir DIR    CMake build directory (default: build-switch)
#   --no-configure     Skip cmake configure (assume already configured)
#   --no-build         Skip build step (just re-stage existing artifacts)
#   --handoff-probe    Include a separate ATF -> BL33 diagnostic entry
#   --bl31 FILE       Install a validated replacement BL31 firmware
#   -h, --help
#
# SD card layout after install:
#   <SD>/bootloader/hekate_ipl.ini      — Hekate L4T boot menu entry
#   <SD>/ame/boot/Image                 — Raw Ame ARM64 Image-style kernel
#   <SD>/ame/boot/uImage                — U-Boot legacy wrapper for bootm
#   <SD>/ame/boot/system.mxb            — Signed Ame system bundle
#   <SD>/ame/boot/system.uInitrd        — U-Boot legacy ramdisk wrapper
#   <SD>/ame/l4t/ame-switch.dtb         — Minimal DTB handed to Ame by U-Boot
#   <SD>/ame/l4t/boot.scr               — U-Boot source/bootm entry
#
# Switchroot U-Boot supplies the required external signed bundle as a legacy
# ramdisk through bootm. The kernel has no embedded-userland fallback.
#
# Prerequisites (first run):
#   cmake -S . -B build-switch -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=scripts/build/toolchain-aarch64.cmake \
#         -DANKO_BOARD=switch-t210
#
# Or just run this script — it auto-configures if build-switch/CMakeCache.txt
# is absent.

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${REPO}/scripts/host.sh"
ame_host_tools
TOOLCHAIN="${REPO}/scripts/build/toolchain-aarch64.cmake"

BUILD_DIR=""
CONFIGURE=1
BUILD=1
HANDOFF_PROBE=0
BL31=""
SD_MOUNT=""

usage() {
    cat <<'EOF'
Usage: scripts/build-hekate-package.sh [options] [SD_MOUNT]

Options:
  --build-dir DIR    CMake build directory (default: build-switch)
  --no-configure     Skip cmake configure
  --no-build         Skip build; re-stage existing artifacts only
  --handoff-probe    Include 'Ame handoff probe' under Hekate's More Configs
  --bl31 FILE       Install a compatible BL31 (build with build-switch-atf.sh)
  -h, --help         Show this help

SD_MOUNT            Optional path to mounted SD card root for installation
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)
            shift
            BUILD_DIR="$1"
            ;;
        --no-configure)
            CONFIGURE=0
            ;;
        --no-build)
            BUILD=0
            ;;
        --handoff-probe)
            HANDOFF_PROBE=1
            ;;
        --bl31)
            [[ $# -ge 2 ]] || { echo "error: --bl31 requires a file" >&2; exit 2; }
            shift
            BL31="$1"
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        /*)
            # Treat absolute paths as SD_MOUNT
            SD_MOUNT="$1"
            ;;
        *)
            echo "error: unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
    shift
done

BUILD_DIR="${BUILD_DIR:-${AME_BUILD_DIR:-${REPO}/build-switch}}"

# Check firmware before any install or expensive build. The old 8 MiB-layout
# BL31 panics against stock Hekate's 1 MiB TZDRAM before reaching U-Boot.
FIRMWARE_SOURCE="${BL31}"
if [[ -z "${FIRMWARE_SOURCE}" && -n "${SD_MOUNT}" ]]; then
    FIRMWARE_SOURCE="${SD_MOUNT%/}/ame/l4t/bl31.bin"
fi
if [[ -n "${FIRMWARE_SOURCE}" ]]; then
    python3 "${REPO}/scripts/switch_atf.py" check "${FIRMWARE_SOURCE}"
fi

# ---------------------------------------------------------------------------
# CMake configure
# ---------------------------------------------------------------------------
if [[ "${CONFIGURE}" == "1" ]]; then
    cmake -S "${REPO}" -B "${BUILD_DIR}" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN}" -DANKO_BOARD=switch-t210
fi
if ! grep -qx 'ANKO_BOARD:STRING=switch-t210' "${BUILD_DIR}/CMakeCache.txt"; then
    echo "error: package build directory is not configured for switch-t210" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
if [[ "${BUILD}" == "1" ]]; then
    BUILD_JOBS="$(ame_jobs)"
    echo "[hekate-pkg] building mochi_system (userland)..."
    cmake --build "${BUILD_DIR}" --target mochi_system --parallel "${BUILD_JOBS}"

    echo "[hekate-pkg] building anko (kernel)..."
    cmake --build "${BUILD_DIR}" --target anko --parallel "${BUILD_JOBS}"
    if [[ "${HANDOFF_PROBE}" == "1" ]]; then
        cmake --build "${BUILD_DIR}" --target switch-handoff --parallel "${BUILD_JOBS}"
    fi
fi

# ---------------------------------------------------------------------------
# Verify artifacts
# ---------------------------------------------------------------------------
IMAGE="${BUILD_DIR}/kernel/anko.bin"
ELF="${BUILD_DIR}/kernel/anko"
SYSTEM_BUNDLE="${BUILD_DIR}/system.mxb"
UIMAGE="${BUILD_DIR}/kernel/anko.uImage"
UINITRD="${BUILD_DIR}/system.uInitrd"
SWITCH_DTS="${REPO}/boards/switch-t210/ame-switch.dts"
SWITCH_DTB="${BUILD_DIR}/ame-switch.dtb"

if [[ ! -f "${IMAGE}" ]]; then
    echo "[hekate-pkg] ERROR: ${IMAGE} not found — build failed?" >&2
    exit 1
fi

if [[ ! -f "${SYSTEM_BUNDLE}" ]]; then
    echo "[hekate-pkg] ERROR: ${SYSTEM_BUNDLE} not found — build failed?" >&2
    exit 1
fi

echo "[hekate-pkg] kernel Image: ${IMAGE} ($(du -h "${IMAGE}" | cut -f1))"
echo "[hekate-pkg] system bundle: ${SYSTEM_BUNDLE} ($(du -h "${SYSTEM_BUNDLE}" | cut -f1))"

# The linked ELF and generated board metadata are the only address authorities.
ENTRY="$(python3 "${REPO}/scripts/check-board-image.py" --build-dir "${BUILD_DIR}" --board switch-t210)"

python3 "${REPO}/scripts/mk-uimage.py" \
        --input "${IMAGE}" \
        --output "${UIMAGE}" \
        --load "${ENTRY}" \
        --entry "${ENTRY}" \
        --type kernel \
        --name Ame
echo "[hekate-pkg] kernel uImage: ${UIMAGE} ($(du -h "${UIMAGE}" | cut -f1))"

python3 "${REPO}/scripts/mk-uimage.py" \
        --input "${SYSTEM_BUNDLE}" \
        --output "${UINITRD}" \
        --load 0 \
        --entry 0 \
        --type ramdisk \
        --name AmeSystem
echo "[hekate-pkg] system uInitrd: ${UINITRD} ($(du -h "${UINITRD}" | cut -f1))"

if ! command -v dtc >/dev/null 2>&1; then
    echo "[hekate-pkg] ERROR: dtc not found; install device-tree-compiler/dtc" >&2
    exit 1
fi
python3 "${REPO}/scripts/switch_atf.py" reservations "${BUILD_DIR}/firmware-reservations.dtsi"
dtc -q -i "${BUILD_DIR}" -I dts -O dtb -o "${SWITCH_DTB}" "${SWITCH_DTS}"
echo "[hekate-pkg] switch DTB: ${SWITCH_DTB} ($(du -h "${SWITCH_DTB}" | cut -f1))"

# ---------------------------------------------------------------------------
# Stage the Hekate package layout in BUILD_DIR/hekate-pkg/
# ---------------------------------------------------------------------------
STAGE="${BUILD_DIR}/hekate-pkg"
rm -rf "${STAGE}"
mkdir -p "${STAGE}/bootloader/ini" "${STAGE}/ame/boot" "${STAGE}/ame/l4t"

cp "${IMAGE}" "${STAGE}/ame/boot/Image"
cp "${UIMAGE}" "${STAGE}/ame/boot/uImage"
cp "${SYSTEM_BUNDLE}" "${STAGE}/ame/boot/system.mxb"
cp "${UINITRD}" "${STAGE}/ame/boot/system.uInitrd"
cp "${SWITCH_DTB}" "${STAGE}/ame/l4t/ame-switch.dtb"
cp "${REPO}/boards/switch-t210/hekate_ipl.ini" "${STAGE}/bootloader/hekate_ipl.ini"
awk '
    $0 == "[Ame]" { copy = 1 }
    copy { print }
' "${REPO}/boards/switch-t210/hekate_ipl.ini" > "${STAGE}/bootloader/ini/ame.ini"
python3 "${REPO}/scripts/mk-uimage.py" \
        --input "${REPO}/boards/switch-t210/boot.cmd" \
        --output "${STAGE}/ame/l4t/boot.scr" \
        --load 0 --entry 0 --type script --name AmeBoot

if [[ -n "${BL31}" ]]; then
    cp "${BL31}" "${STAGE}/ame/l4t/bl31.bin"
fi

if [[ "${HANDOFF_PROBE}" == "1" ]]; then
    mkdir -p "${STAGE}/ame/handoff"
    cp "${BUILD_DIR}/kernel/switch-handoff.bin" "${STAGE}/ame/handoff/bl33.bin"
    cp "${REPO}/boards/switch-t210/handoff/hekate.ini" \
        "${STAGE}/bootloader/ini/ame-handoff.ini"
    if [[ -n "${FIRMWARE_SOURCE}" ]]; then
        cp "${FIRMWARE_SOURCE}" "${STAGE}/ame/handoff/bl31.bin"
    fi
    echo "[hekate-pkg] staged handoff probe (uses the same validated BL31 as Ame)"
fi

echo "[hekate-pkg] staged:"
echo "  ${STAGE}/bootloader/hekate_ipl.ini"
echo "  ${STAGE}/bootloader/ini/ame.ini"
echo "  ${STAGE}/ame/boot/Image"
echo "  ${STAGE}/ame/boot/uImage"
echo "  ${STAGE}/ame/boot/system.mxb"
echo "  ${STAGE}/ame/boot/system.uInitrd"
echo "  ${STAGE}/ame/l4t/ame-switch.dtb"
echo "  ${STAGE}/ame/l4t/boot.scr"

# ---------------------------------------------------------------------------
# SD card install (optional)
# ---------------------------------------------------------------------------
if [[ -z "${SD_MOUNT}" ]]; then
    echo ""
    echo "[hekate-pkg] SD_MOUNT not given — skipping SD card install."
    echo "[hekate-pkg] To install, run:"
    probe_option=""
    [[ "${HANDOFF_PROBE}" == "1" ]] && probe_option=" --handoff-probe"
    [[ -n "${BL31}" ]] && probe_option="${probe_option} --bl31 \"${BL31}\""
    echo "  $0 --build-dir \"${BUILD_DIR}\" --no-build${probe_option} \"/Volumes/SD_NAME\""
    echo "The installer also removes the obsolete Ame extlinux entry."
    exit 0
fi

if [[ ! -d "${SD_MOUNT}" ]]; then
    echo "[hekate-pkg] ERROR: SD mount point '${SD_MOUNT}' not found." >&2
    exit 1
fi

echo "[hekate-pkg] installing to ${SD_MOUNT}..."
rsync -a "${STAGE}/" "${SD_MOUNT%/}/"
rm -f "${SD_MOUNT%/}/bootloader/payloads/anko.bin"
# Retire the old Ame-generated entry: U-Boot scans extlinux before boot.scr.
rm -f "${SD_MOUNT%/}/ame/l4t/extlinux/extlinux.conf"

echo "[hekate-pkg] done."
echo ""
echo "Eject SD card, insert into Switch, launch Hekate, select 'Ame'."
if [[ "${HANDOFF_PROBE}" == "1" ]]; then
    echo "For the diagnostic, select More Configs -> Ame handoff probe (intentional magenta halt)."
fi
echo "Requires Switchroot/L4T shared firmware under bootloader/sys/l4t/ and bl31.bin/bl33.bin under ame/l4t/."
