#!/usr/bin/env bash
set -euo pipefail

AME="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${DANGO_LINUX_OUT:-${AME}/build/dango-linux}"
: "${DANGO_LINUX_BOOTARGS:=console=ttyAMA0,115200n8 earlycon=pl011,0x09000000 loglevel=4 nosmp maxcpus=1 cma=0 coherent_pool=1M cpuidle.off=1 rdinit=/init panic=-1}"

"${AME}/scripts/linux/check-artifacts.sh" "${OUT}" >/dev/null

exec qemu-system-aarch64 \
    -M virt \
    -cpu cortex-a53 \
    -m 128M \
    -nographic \
    -kernel "${OUT}/Image" \
    -initrd "${OUT}/initramfs.cpio" \
    -append "${DANGO_LINUX_BOOTARGS}"
