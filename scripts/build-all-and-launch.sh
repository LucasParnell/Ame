#!/usr/bin/env bash
# Build Ame completely, refresh the non-legacy U-Boot/disk image, and launch it.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${ROOT}/scripts/host.sh"
ame_host_tools
BUILD_DIR="${AME_BUILD_DIR:-${ROOT}/build}"
TOOLCHAIN="${AME_TOOLCHAIN_FILE:-${ROOT}/scripts/build/toolchain-aarch64.cmake}"
AUTO_BUILD_UBOOT="${AME_AUTO_BUILD_UBOOT:-1}"
DANGO_BUILD_LINUX="${DANGO_BUILD_LINUX:-0}"
DANGO_REQUIRE_LINUX="${DANGO_REQUIRE_LINUX:-${DANGO_BUILD_LINUX}}"
LAUNCH=1
DEBUG=0
HEADLESS=0
BOARD="${ANKO_BOARD:-qemu-virt}"
BOOT_MODE="${AME_BOOT_MODE:-direct}"

usage() {
    cat <<'EOF'
Usage: scripts/build-all-and-launch.sh [options]

Options:
  --debug             Launch QEMU paused for gdb on :1234.
  --headless          Suppress the guest terminal and graphical display.
  --boot-mode MODE    direct (default) or disk (requires U-Boot).
  --no-launch         Build everything and refresh the disk, but do not run QEMU.
  --no-uboot-build    Do not auto-build local U-Boot if no firmware is found.
  --board BOARD       Target board: qemu-virt (default) or switch-t210.
  -h, --help          Show this help.

Environment:
  AME_BUILD_DIR       CMake build directory. Default: ./build
  AME_BUILD_JOBS      Parallel build jobs. Default: nproc when available
  AME_DISK            Output disk image. Default: $AME_BUILD_DIR/ame-disk.img
  DANGO_BUILD_LINUX   Build/package minimal Linux payload. Default: 0
  DANGO_LINUX_ARTIFACT_DIR
                      Use prebuilt Linux payload from this directory instead of building it.
  DANGO_REQUIRE_LINUX Fail if system.mxb lacks the Linux payload. Default: $DANGO_BUILD_LINUX
  ANKO_BOARD          Target board (qemu-virt|switch-t210). Default: qemu-virt.
  UBOOT_BIN           Explicit U-Boot firmware for QEMU.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --debug|debug)
            DEBUG=1
            ;;
        --headless)
            HEADLESS=1
            ;;
        --boot-mode)
            shift
            BOOT_MODE="${1:-}"
            ;;
        --no-launch)
            LAUNCH=0
            ;;
        --no-uboot-build)
            AUTO_BUILD_UBOOT=0
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --board)
            shift
            BOARD="${1:-}"
            [[ -z "${BOARD}" ]] && { echo "error: --board requires an argument" >&2; exit 2; }
            ;;
        *)
            echo "error: unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
    shift
done
case "${BOOT_MODE}" in
    direct|disk) ;;
    *) echo 'error: boot mode must be direct or disk' >&2; exit 2 ;;
esac

need() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "error: required tool not found: $1" >&2
        exit 1
    fi
}

find_uboot() {
    if [[ -n "${UBOOT_BIN:-}" ]]; then
        [[ -f "${UBOOT_BIN}" ]] && printf '%s\n' "${UBOOT_BIN}"
        return 0
    fi

    local candidate
    for candidate in \
        "${BUILD_DIR}/u-boot-qemu-arm64.bin" \
        "${ROOT}/build/u-boot-qemu-arm64.bin" \
        /usr/share/u-boot/qemu_arm64/u-boot.bin \
        /usr/share/u-boot/qemu-arm64/u-boot.bin \
        /usr/lib/u-boot/qemu_arm64/u-boot.bin \
        /usr/lib/u-boot/qemu-arm64/u-boot.bin
    do
        if [[ -f "${candidate}" ]]; then
            printf '%s\n' "${candidate}"
            return 0
        fi
    done
    return 1
}

# For switch-t210 delegate to the Hekate package builder.
if [[ "${BOARD}" == "switch-t210" ]]; then
    SWITCH_BUILD_DIR="${AME_BUILD_DIR:-${ROOT}/build-switch}"
    exec "${ROOT}/scripts/build-hekate-package.sh" \
        --build-dir "${SWITCH_BUILD_DIR}" \
        ${SD_MOUNT:+"${SD_MOUNT}"}
fi

need cmake

verify_dango_system_bundle() {
    local bundle="${BUILD_DIR}/system.mxb"
    if [[ ! -f "${bundle}" ]]; then
        echo "error: missing ${bundle}" >&2
        exit 1
    fi

    local listing
    listing="$(mktemp)"
    python3 "${ROOT}/scripts/mx/mxpack.py" list "${bundle}" >"${listing}"

    local required=(
        linux/manifest.toml
        linux/Image
        linux/initramfs.cpio
        linux/dango-linux.dtb
    )
    local name
    for name in "${required[@]}"; do
        if ! grep -qx "${name}" "${listing}"; then
            rm -f "${listing}"
            echo "error: ${bundle} does not contain ${name}" >&2
            echo "hint: configure with -DDANGO_BUILD_LINUX_ARTIFACTS=ON or set DANGO_LINUX_ARTIFACT_DIR=/path/to/dango-linux" >&2
            exit 1
        fi
    done
    rm -f "${listing}"
}

cmake_configure_args() {
    local args=(
        -S "${ROOT}"
        -B "${BUILD_DIR}"
        -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN}"
        -DANKO_BOARD="${BOARD}"
    )

    if [[ -n "${DANGO_LINUX_ARTIFACT_DIR:-}" ]]; then
        args+=(-DDANGO_LINUX_ARTIFACT_DIR="${DANGO_LINUX_ARTIFACT_DIR}")
    elif [[ "${DANGO_BUILD_LINUX}" == "1" ]]; then
        args+=(-DDANGO_BUILD_LINUX_ARTIFACTS=ON)
    else
        args+=(-DDANGO_BUILD_LINUX_ARTIFACTS=OFF)
    fi

    printf '%s\0' "${args[@]}"
}

mkdir -p "${BUILD_DIR}"

CONFIGURE_ARGS=()
while IFS= read -r -d '' arg; do
    CONFIGURE_ARGS+=("${arg}")
done < <(cmake_configure_args)

if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
    echo "==> Configuring ${BUILD_DIR}"
else
    echo "==> Updating CMake configuration for ${BUILD_DIR}"
fi
cmake "${CONFIGURE_ARGS[@]}"

BUILD_PARALLEL=(--parallel "$(ame_jobs)")

echo "==> Building all default targets"
DANGO_REQUIRE_LINUX="${DANGO_REQUIRE_LINUX}" \
cmake --build "${BUILD_DIR}" "${BUILD_PARALLEL[@]}"

if [[ "${DANGO_REQUIRE_LINUX}" == "1" ]]; then
    echo "==> Verifying Linux payload is packaged in system.mxb"
    verify_dango_system_bundle
fi

if [[ "${BOOT_MODE}" == disk ]]; then
echo "==> Building Ame disk image"
AME_BUILD_DIR="${BUILD_DIR}" \
AME_DISK="${AME_DISK:-${BUILD_DIR}/ame-disk.img}" \
DANGO_REQUIRE_LINUX="${DANGO_REQUIRE_LINUX}" \
cmake --build "${BUILD_DIR}" --target ame_disk "${BUILD_PARALLEL[@]}"

UBOOT="$(find_uboot || true)"
if [[ -z "${UBOOT}" && "${AUTO_BUILD_UBOOT}" == "1" ]]; then
    echo "==> U-Boot firmware not found; building local qemu_arm64 U-Boot"
    AME_BUILD_DIR="${BUILD_DIR}" \
    "${ROOT}/scripts/build-u-boot-qemu-arm64.sh"
    UBOOT="$(find_uboot || true)"
fi
fi

if [[ "${LAUNCH}" == "0" ]]; then
    echo "==> Build complete; launch skipped"
    exit 0
fi

if [[ "${BOOT_MODE}" == disk && -z "${UBOOT:-}" ]]; then
    echo "error: U-Boot for qemu_arm64 not found." >&2
    echo "hint: set UBOOT_BIN=/path/to/u-boot.bin or rerun without --no-uboot-build." >&2
    exit 1
fi

echo "==> Launching ${BOOT_MODE} boot"
RUN_ENV=(
    env
    AME_BOOT_MODE="${BOOT_MODE}"
    AME_BUILD_DISK=0
    AME_BUILD_DIR="${BUILD_DIR}"
    AME_DISK="${AME_DISK:-${BUILD_DIR}/ame-disk.img}"
    DANGO_REQUIRE_LINUX="${DANGO_REQUIRE_LINUX}"
    UBOOT_BIN="${UBOOT:-}"
)

if [[ "${HEADLESS}" == "1" ]]; then
    RUN_ENV+=(DANGO_GUEST_TERMINAL=0 QEMU_DISPLAY=none)
fi

if [[ "${DEBUG}" == "1" ]]; then
    exec "${RUN_ENV[@]}" "${ROOT}/scripts/run-qemu.sh" debug
fi

exec "${RUN_ENV[@]}" "${ROOT}/scripts/run-qemu.sh"
