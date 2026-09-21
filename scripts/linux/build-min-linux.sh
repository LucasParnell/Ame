#!/usr/bin/env bash
set -euo pipefail

AME="$(cd "$(dirname "$0")/../.." && pwd)"
source "${AME}/scripts/linux/dango-load.env"
: "${DANGO_LINUX_SRC:=$HOME/Code/linux}"
: "${DANGO_LINUX_TAG:=v6.12}"
export PATH="${AME}/scripts/linux/toolbin:${PATH}"

OUT="${DANGO_LINUX_OUT:-${AME}/build/dango-linux}"
KOUT="${DANGO_LINUX_SRC}/out-dango"
: "${DANGO_LINUX_BOOTARGS:=console=ttyAMA0,115200n8 earlycon=pl011,0x09000000 loglevel=4 nosmp maxcpus=1 cma=0 coherent_pool=1M cpuidle.off=1 rdinit=/init panic=-1}"
mkdir -p "${OUT}"

if [[ ! -d "${DANGO_LINUX_SRC}/.git" ]]; then
    mkdir -p "$(dirname "${DANGO_LINUX_SRC}")"
    git clone --branch "${DANGO_LINUX_TAG}" --depth=1 \
        https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git \
        "${DANGO_LINUX_SRC}"
fi

if [[ ! -f "${DANGO_LINUX_SRC}/Makefile" ]]; then
    echo "error: DANGO_LINUX_SRC does not look like a Linux source tree: ${DANGO_LINUX_SRC}" >&2
    exit 1
fi

echo "[1/5] build dango-agent"
"${AME}/scripts/linux/build-agent.sh"

echo "[2/5] build initramfs"
"${AME}/scripts/linux/build-initramfs.sh"

echo "[3/5] configure/build linux"
if [[ "${DANGO_LINUX_TRACE:-0}" != "0" ]]; then
    DANGO_LINUX_SRC="${DANGO_LINUX_SRC}" "${AME}/scripts/linux/apply-dango-trace.py" "${DANGO_LINUX_SRC}"
fi
# Start from defconfig for the first reliable guest boot, then apply the
# hello-image fragment. The fragment removes obvious subsystems but lets Linux
# keep whatever low-level options it needs to boot on arm64/QEMU virt.
if [[ ! -f "${KOUT}/.dango-configured" || "${AME}/linux-artifacts/config/dango_hello_arm64.fragment" -nt "${KOUT}/.dango-configured" ]]; then
    rm -f "${KOUT}/.dango-configured"
    make LLVM=1 ARCH=arm64 O="${KOUT}" -C "${DANGO_LINUX_SRC}" defconfig
    "${DANGO_LINUX_SRC}/scripts/kconfig/merge_config.sh" \
        -m \
        -O "${KOUT}" \
        "${KOUT}/.config" \
        "${AME}/linux-artifacts/config/dango_hello_arm64.fragment"
    make LLVM=1 ARCH=arm64 O="${KOUT}" -C "${DANGO_LINUX_SRC}" olddefconfig
    touch "${KOUT}/.dango-configured"
fi

make LLVM=1 ARCH=arm64 O="${KOUT}" -C "${DANGO_LINUX_SRC}" -j"$(nproc)" Image

cp "${KOUT}/arch/arm64/boot/Image" "${OUT}/Image"
cp "${KOUT}/vmlinux" "${OUT}/vmlinux" || true
cp "${KOUT}/System.map" "${OUT}/System.map" || true

echo "[4/5] build dtb"
# Build the DTB after initramfs exists so linux,initrd-end is correct.
DANGO_LINUX_SRC="${DANGO_LINUX_SRC}" "${AME}/scripts/linux/build-dtb.sh"

echo "[5/5] write manifest"
image_size=$(stat -c%s "${OUT}/Image")
initramfs_size=$(stat -c%s "${OUT}/initramfs.cpio")
dtb_size=$(stat -c%s "${OUT}/dango-linux.dtb")
image_text_offset=$(python3 - "${OUT}/Image" <<'PY_IMAGE_HEADER'
import pathlib
import struct
import sys
image = pathlib.Path(sys.argv[1]).read_bytes()[:64]
if len(image) < 64 or image[56:60] != b'ARMd':
    print('0')
else:
    print(str(struct.unpack_from('<Q', image, 8)[0]))
PY_IMAGE_HEADER
)

if [[ -n "${DANGO_LINUX_KERNEL_LOAD:-}" ]]; then
    KERNEL_LOAD="${DANGO_LINUX_KERNEL_LOAD}"
elif [[ "${image_text_offset}" == "0" ]]; then
    # Modern arm64 Images often report text_offset=0 and require a 2 MiB-aligned
    # placement. Leave the first 2 MiB of guest RAM free for the tiny Dango
    # diagnostic trampoline and load Linux at the next 2 MiB boundary.
    KERNEL_LOAD="0x40200000"
else
    # Older arm64 Images use a non-zero text_offset, commonly 0x80000. In that
    # case the conventional placement is RAM_BASE + text_offset.
    KERNEL_LOAD=$(printf '0x%x' $((0x40000000 + image_text_offset)))
fi
echo "dango: linux Image text_offset=$(printf '0x%x' "${image_text_offset}") kernel_load=${KERNEL_LOAD}"

cat > "${OUT}/manifest.toml" <<EOF_MANIFEST
name = "dango-linux-hello"
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
initramfs_load = "${DANGO_INITRAMFS_LOAD}"
dtb_load = "${DANGO_DTB_LOAD}"

entry = "kernel_load"
bootargs = "${DANGO_LINUX_BOOTARGS}"
vcpus = 1
EOF_MANIFEST

"${AME}/scripts/linux/check-artifacts.sh" "${OUT}"
echo "dango: linux manifest entry=$(awk -F= '/^entry[[:space:]]*=/{gsub(/[ "\t]/, "", $2); print $2}' "${OUT}/manifest.toml") kernel_load=${KERNEL_LOAD}"
echo "dango: linux artifacts ready"
