#!/usr/bin/env bash
# scripts/run-qemu.sh — boot Ame under QEMU virt.
#
# Usage:
#   scripts/run-qemu.sh           # run
#   scripts/run-qemu.sh debug     # stop on entry, wait for gdb on :1234
#
# Guest console:
#   By default, guest serial output is written to build/dango-guest.log and a
#   terminal is opened with tail -f. Set DANGO_GUEST_TERMINAL=0 to suppress the
#   terminal, DANGO_GUEST_LOG=/path/to/log to choose the file, or
#   DANGO_GUEST_SERIAL=pty to use a raw QEMU serial backend.
#
# Prereqs:
#   - cmake build complete:
#       cmake -B build -DCMAKE_TOOLCHAIN_FILE=scripts/build/toolchain-aarch64.cmake
#       cmake --build build
#       cmake --build build --target ame_disk
#   - qemu-system-aarch64 in PATH
#   - U-Boot for qemu_arm64 in UBOOT_BIN, or installed in a common distro path
#   - Raw U-Boot commands are generated in build/boot.raw.cmd; this disk does
#     not contain a FAT/extlinux boot partition.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${ROOT}/scripts/host.sh"
ame_host_tools
DEFAULT_DISPLAY=sdl
if [[ "$(uname -s)" == Darwin ]]; then DEFAULT_DISPLAY=cocoa; fi
case "${AME_BOOT_MODE:-direct}" in
    direct|disk) ;;
    *) echo 'error: AME_BOOT_MODE must be direct or disk' >&2; exit 2 ;;
esac
BUILD_DIR="${AME_BUILD_DIR:-${ROOT}/build}"
KERNEL="${BUILD_DIR}/kernel/anko.bin"
INITRD="${BUILD_DIR}/system.mxb"
SYSTEM_BUNDLE="${BUILD_DIR}/system.mxb"
GUEST_LOG="${DANGO_GUEST_LOG:-${BUILD_DIR}/dango-guest.log}"
FS_DISK="${AME_FS_DISK:-${BUILD_DIR}/fs.img}"
AME_DISK="${AME_DISK:-${BUILD_DIR}/ame-disk.img}"
FS_DISK_SIZE="${AME_FS_DISK_SIZE:-128M}"

find_uboot() {
    if [[ -n "${UBOOT_BIN:-}" ]]; then
        printf '%s\n' "${UBOOT_BIN}"
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

require_mxb_object() {
    local bundle="$1" object="$2"
    if ! python3 "${ROOT}/scripts/mx/mxpack.py" list "${bundle}" | grep -qx "${object}"; then
        echo "error: ${bundle} does not contain ${object}" >&2
        echo "hint: configure with -DDANGO_BUILD_LINUX_ARTIFACTS=ON or set DANGO_LINUX_ARTIFACT_DIR=/path/to/dango-linux" >&2
        exit 1
    fi
}

require_dango_linux_payload() {
    local bundle="$1"
    require_mxb_object "${bundle}" linux/manifest.toml
    require_mxb_object "${bundle}" linux/Image
    require_mxb_object "${bundle}" linux/initramfs.cpio
    require_mxb_object "${bundle}" linux/dango-linux.dtb
}

open_guest_terminal() {
    local log="$1"
    local title="Ame guest console"
    local quoted_log
    printf -v quoted_log '%q' "${log}"
    local cmd="tail -n +1 -f ${quoted_log}"

    if [[ "${DANGO_GUEST_TERMINAL:-1}" == "0" ]]; then
        return 0
    fi

    echo "guest console: ${log}" >&2
    if [[ -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" ]]; then
        echo "open another terminal and run: tail -f ${log}" >&2
        return 0
    fi

    if command -v x-terminal-emulator >/dev/null 2>&1; then
        x-terminal-emulator -T "${title}" -e sh -lc "${cmd}" >/dev/null 2>&1 &
    elif command -v gnome-terminal >/dev/null 2>&1; then
        gnome-terminal --title="${title}" -- sh -lc "${cmd}" >/dev/null 2>&1 &
    elif command -v konsole >/dev/null 2>&1; then
        konsole --new-tab -p "tabtitle=${title}" -e sh -lc "${cmd}" >/dev/null 2>&1 &
    elif command -v xfce4-terminal >/dev/null 2>&1; then
        xfce4-terminal --title="${title}" --command "sh -lc '${cmd}'" >/dev/null 2>&1 &
    elif command -v mate-terminal >/dev/null 2>&1; then
        mate-terminal --title="${title}" -- sh -lc "${cmd}" >/dev/null 2>&1 &
    elif command -v lxterminal >/dev/null 2>&1; then
        lxterminal --title="${title}" -e sh -lc "${cmd}" >/dev/null 2>&1 &
    elif command -v alacritty >/dev/null 2>&1; then
        alacritty --title "${title}" -e sh -lc "${cmd}" >/dev/null 2>&1 &
    elif command -v kitty >/dev/null 2>&1; then
        kitty --title "${title}" sh -lc "${cmd}" >/dev/null 2>&1 &
    elif command -v foot >/dev/null 2>&1; then
        foot --title="${title}" sh -lc "${cmd}" >/dev/null 2>&1 &
    elif command -v xterm >/dev/null 2>&1; then
        xterm -T "${title}" -e sh -lc "${cmd}" >/dev/null 2>&1 &
    else
        echo "open another terminal and run: tail -f ${log}" >&2
    fi
}

prepare_guest_serial() {
    if [[ -n "${DANGO_GUEST_SERIAL:-}" ]]; then
        if [[ "${DANGO_GUEST_SERIAL}" == file:* ]]; then
            GUEST_LOG="${DANGO_GUEST_SERIAL#file:}"
            mkdir -p "$(dirname "${GUEST_LOG}")"
            : > "${GUEST_LOG}"
            open_guest_terminal "${GUEST_LOG}"
        fi
        printf '%s\n' "${DANGO_GUEST_SERIAL}"
        return 0
    fi

    mkdir -p "$(dirname "${GUEST_LOG}")"
    : > "${GUEST_LOG}"
    open_guest_terminal "${GUEST_LOG}"
    printf 'file:%s\n' "${GUEST_LOG}"
}

GUEST_SERIAL="$(prepare_guest_serial)"

if [[ "${AME_BOOT_MODE:-direct}" == "direct" ]]; then
    if [[ ! -f "${KERNEL}" ]]; then
        echo "error: ${KERNEL} not found — run cmake build first" >&2
        exit 1
    fi

    if [[ ! -f "${INITRD}" ]]; then
        echo "error: ${INITRD} not found — run cmake build first" >&2
        exit 1
    fi

    if [[ "${DANGO_REQUIRE_LINUX:-0}" == "1" ]]; then
        if [[ "${INITRD}" == *.mxb ]]; then
            require_dango_linux_payload "${INITRD}"
        else
            for object in linux/manifest.toml linux/Image linux/initramfs.cpio linux/dango-linux.dtb; do
                if ! tar -tf "${INITRD}" | grep -qx "${object}"; then
                    echo "error: ${INITRD} does not contain ${object}" >&2
                    exit 1
                fi
            done
        fi
    fi

    mkdir -p "$(dirname "${FS_DISK}")"
    if [[ "${AME_INSTALL_VFS:-0}" == "1" ]]; then
        AME_BUILD_DIR="${BUILD_DIR}" \
        AME_FS_DISK="${FS_DISK}" \
        AME_FS_DISK_SIZE="${FS_DISK_SIZE}" \
        "${ROOT}/scripts/install-ame-vfs.sh"
    elif [[ ! -f "${FS_DISK}" ]]; then
        ame_sparse_file "${FS_DISK}" "${FS_DISK_SIZE}"
    fi

    QEMU_ARGS=(
        -machine virt,virtualization=on,gic-version=2
        -cpu cortex-a57
        -smp "${QEMU_SMP:-2}"
        -m "${QEMU_MEM:-512M}"
        -device ramfb
        -drive if=none,file="${FS_DISK}",format=raw,id=fsdisk
        -device virtio-blk-device,drive=fsdisk
        -device qemu-xhci,id=xhci
        -device usb-kbd,bus=xhci.0
        -device usb-mouse,bus=xhci.0
        -display "${QEMU_DISPLAY:-${DEFAULT_DISPLAY}}"
        -serial mon:stdio
        -serial "${GUEST_SERIAL}"
        -kernel "${KERNEL}"
        -initrd "${INITRD}"
    )
else
    if [[ "${AME_BUILD_DISK:-0}" == "1" || ! -f "${AME_DISK}" ]]; then
        AME_BUILD_DIR="${BUILD_DIR}" \
        AME_DISK="${AME_DISK}" \
        "${ROOT}/scripts/build-ame-disk.sh"
    fi
    if [[ ! -f "${AME_DISK}" ]]; then
        echo "error: ${AME_DISK} not found — run cmake --build ${BUILD_DIR} --target ame_disk" >&2
        exit 1
    fi
    UBOOT="$(find_uboot || true)"
    if [[ -z "${UBOOT}" ]]; then
        echo "error: U-Boot for qemu_arm64 not found; set UBOOT_BIN=/path/to/u-boot.bin" >&2
        echo "hint: set AME_BOOT_MODE=direct to use the old -kernel/-initrd development path" >&2
        exit 1
    fi
    if [[ "${DANGO_REQUIRE_LINUX:-0}" == "1" ]]; then
        require_file_bundle="${SYSTEM_BUNDLE}"
        if [[ ! -f "${require_file_bundle}" ]]; then
            echo "error: ${require_file_bundle} not found — run cmake --build ${BUILD_DIR} --target mochi_system" >&2
            exit 1
        fi
        require_dango_linux_payload "${require_file_bundle}"
    fi

    if [[ -f "${BUILD_DIR}/boot.raw.cmd" ]]; then
        echo "raw U-Boot boot commands: ${BUILD_DIR}/boot.raw.cmd" >&2
    fi

    QEMU_ARGS=(
        -machine virt,virtualization=on,gic-version=2
        -cpu cortex-a57
        -smp "${QEMU_SMP:-2}"
        -m "${QEMU_MEM:-512M}"
        -device ramfb
        -drive if=none,file="${AME_DISK}",format=raw,id=amedisk
        -device virtio-blk-device,drive=amedisk
        -device qemu-xhci,id=xhci
        -device usb-kbd,bus=xhci.0
        -device usb-mouse,bus=xhci.0
        -display "${QEMU_DISPLAY:-${DEFAULT_DISPLAY}}"
        -serial mon:stdio
        -serial "${GUEST_SERIAL}"
        -bios "${UBOOT}"
    )
fi

if [[ "${1:-}" == "debug" ]]; then
    QEMU_ARGS+=(-S -gdb tcp::1234)
    echo "QEMU paused, waiting for gdb on :1234"
fi

exec qemu-system-aarch64 "${QEMU_ARGS[@]}"
