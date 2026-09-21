#!/usr/bin/env bash
#
# scripts/linux/build-manifest.sh — write <artifact-dir>/manifest.toml for a
# built Linux appliance artifact set and validate it.
#
# Extracted from build-gpu-linux.sh so that iterating on the guest userspace
# (which rebuilds only the agent / initramfs / DTB, not the kernel) regenerates
# a manifest with matching sizes instead of leaving a stale one behind, which
# check-profile.py rejects at signing time.
#
# Env:
#   DANGO_LINUX_OUT          artifact dir (default: <repo>/build/dango-linux)
#   DANGO_LINUX_PROFILE      profile written to the manifest (default: hello)
#   DANGO_MANIFEST_NAME      manifest "name" field (default: dango-linux-<profile>)
#   DANGO_LINUX_BOOTARGS     bootargs (default: the appliance default)
#   DANGO_LINUX_KERNEL_LOAD  explicit kernel load address override
set -euo pipefail

AME="$(cd "$(dirname "$0")/../.." && pwd)"
source "${AME}/scripts/linux/dango-load.env"
OUT="${DANGO_LINUX_OUT:-${AME}/build/dango-linux}"
PROFILE="${DANGO_LINUX_PROFILE:-hello}"
NAME="${DANGO_MANIFEST_NAME:-dango-linux-${PROFILE}}"
: "${DANGO_LINUX_BOOTARGS:=console=ttyAMA0,115200n8 earlycon=pl011,0x09000000 loglevel=4 nosmp maxcpus=1 cma=0 coherent_pool=1M cpuidle.off=1 rdinit=/init panic=-1}"

filesize() { python3 -c 'import os,sys; print(os.path.getsize(sys.argv[1]))' "$1"; }
image_size=$(filesize "${OUT}/Image")
initramfs_size=$(filesize "${OUT}/initramfs.cpio")
dtb_size=$(filesize "${OUT}/dango-linux.dtb")

initramfs_load=$((DANGO_INITRAMFS_LOAD))
dtb_load=$((DANGO_DTB_LOAD))
if (( initramfs_load + initramfs_size > dtb_load )); then
    echo "error: initramfs overlaps the DTB load address" >&2
    exit 1
fi

# Modern arm64 Images report text_offset=0 and require 2 MiB-aligned placement;
# the tiny Dango diagnostic trampoline keeps the first 2 MiB of guest RAM, so
# Linux loads at the next 2 MiB boundary. Older Images carry a non-zero
# text_offset and are placed at RAM_BASE + text_offset.
image_text_offset=$(python3 - "${OUT}/Image" <<'PY_IMAGE_HEADER'
import pathlib, struct, sys
image = pathlib.Path(sys.argv[1]).read_bytes()[:64]
print(str(struct.unpack_from('<Q', image, 8)[0]) if len(image) >= 64 and image[56:60] == b'ARMd' else '0')
PY_IMAGE_HEADER
)

if [[ -n "${DANGO_LINUX_KERNEL_LOAD:-}" ]]; then
    KERNEL_LOAD="${DANGO_LINUX_KERNEL_LOAD}"
elif [[ "${image_text_offset}" == "0" ]]; then
    KERNEL_LOAD="0x40200000"
else
    KERNEL_LOAD=$(printf '0x%x' $((0x40000000 + image_text_offset)))
fi

cat > "${OUT}/manifest.toml" <<EOF_MANIFEST
name = "${NAME}"
profile = "${PROFILE}"
arch = "aarch64"

kernel = "Image"
dtb = "dango-linux.dtb"
initramfs = "initramfs.cpio"

kernel_size = ${image_size}
initramfs_size = ${initramfs_size}
dtb_size = ${dtb_size}

memory_base = "0x40000000"
memory_size = "0x08000000"

kernel_load = "${KERNEL_LOAD}"
initramfs_load = "${initramfs_load}"
dtb_load = "${dtb_load}"

entry = "kernel_load"
bootargs = "${DANGO_LINUX_BOOTARGS}"
vcpus = 1
EOF_MANIFEST

"${AME}/scripts/linux/check-artifacts.sh" "${OUT}"
echo "dango: ${PROFILE} appliance artifacts ready in ${OUT}"
