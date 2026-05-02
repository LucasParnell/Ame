#!/usr/bin/env bash
# scripts/run-qemu.sh — boot the anko kernel under QEMU virt.
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
#   - qemu-system-aarch64 in PATH

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KERNEL="${ROOT}/build/kernel/anko"
INITRD="${ROOT}/build/initrd.tar"
GUEST_LOG="${DANGO_GUEST_LOG:-${ROOT}/build/dango-guest.log}"

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

if [[ ! -f "${KERNEL}" ]]; then
    echo "error: ${KERNEL} not found — run cmake build first" >&2
    exit 1
fi

if [[ ! -f "${INITRD}" ]]; then
    echo "error: ${INITRD} not found — run cmake build first" >&2
    exit 1
fi

if [[ "${DANGO_REQUIRE_LINUX:-0}" == "1" ]]; then
    if ! tar -tf "${INITRD}" | grep -qx 'linux/Image'; then
        echo "error: initrd.tar does not contain linux/Image" >&2
        echo "Configure with -DDANGO_BUILD_LINUX_ARTIFACTS=ON or -DDANGO_LINUX_ARTIFACT_DIR=/path/to/build/dango-linux" >&2
        exit 1
    fi
    if ! tar -tf "${INITRD}" | grep -qx 'linux/initramfs.cpio'; then
        echo "error: initrd.tar does not contain linux/initramfs.cpio" >&2
        exit 1
    fi
fi

GUEST_SERIAL="$(prepare_guest_serial)"

QEMU_ARGS=(
    -machine virt,virtualization=on,gic-version=2
    -cpu cortex-a57
    -smp 2
    -m "${QEMU_MEM:-512M}"
    -nographic
    -serial mon:stdio
    -serial "${GUEST_SERIAL}"
    -kernel "${KERNEL}"
    -initrd "${INITRD}"
)

if [[ "${1:-}" == "debug" ]]; then
    QEMU_ARGS+=(-S -gdb tcp::1234)
    echo "QEMU paused, waiting for gdb on :1234"
fi

exec qemu-system-aarch64 "${QEMU_ARGS[@]}"
