#!/usr/bin/env bash
# Build the QEMU disk used by the full-disk Ame boot path.
#
# Layout:
#   GPT partition 1: raw AArch64 kernel image for U-Boot `virtio read`
#   GPT partition 2: AmeFS root containing /ame/boot/system.mxb

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${ROOT}/scripts/host.sh"
ame_host_tools
BUILD_DIR="${AME_BUILD_DIR:-${ROOT}/build}"
DISK="${AME_DISK:-${BUILD_DIR}/ame-disk.img}"
DISK_SIZE="${AME_DISK_SIZE:-256M}"
BOOT_PART_SIZE="${AME_BOOT_PART_SIZE:-64M}"
ROOT_IMG="${BUILD_DIR}/ame-root.img"

need() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "error: required tool not found: $1" >&2
        exit 1
    fi
}

require_file() {
    if [[ ! -f "$1" ]]; then
        echo "error: missing build artifact: $1" >&2
        echo "hint: run cmake --build ${BUILD_DIR}" >&2
        exit 1
    fi
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
    require_mxb_object "${SYSTEM_BUNDLE}" linux/manifest.toml
    require_mxb_object "${SYSTEM_BUNDLE}" linux/Image
    require_mxb_object "${SYSTEM_BUNDLE}" linux/initramfs.cpio
    require_mxb_object "${SYSTEM_BUNDLE}" linux/dango-linux.dtb
}

part_field() {
    local part="$1" field="$2"
    sgdisk -i "${part}" "${DISK}" |
        awk -F: -v key="${field}" '$1 ~ key { gsub(/^[ \t]+|[ \t]+$/, "", $2); split($2, a, /[ \t]+/); print a[1]; exit }'
}

need sgdisk
need dd
need python3

KERNEL_BIN="${BUILD_DIR}/kernel/anko.bin"
SYSTEM_BUNDLE="${BUILD_DIR}/system.mxb"
MANIFEST_JSON="${BUILD_DIR}/manifest.json"

require_file "${KERNEL_BIN}"
require_file "${SYSTEM_BUNDLE}"
require_file "${MANIFEST_JSON}"

if [[ "${DANGO_REQUIRE_LINUX:-0}" == "1" ]]; then
    require_dango_linux_payload
fi

mkdir -p "$(dirname "${DISK}")"
rm -f "${DISK}" "${ROOT_IMG}"
ame_sparse_file "${DISK}" "${DISK_SIZE}"

sgdisk --clear \
       --set-alignment=1 \
       --new=1:2048:+"${BOOT_PART_SIZE}" \
       --typecode=1:8300 \
       --change-name=1:AMEBOOT_RAW \
       --new=2:0:0 \
       --typecode=2:8300 \
       --change-name=2:AMEROOT \
       "${DISK}" >/dev/null

BOOT_FIRST="$(part_field 1 'First sector')"
BOOT_LAST="$(part_field 1 'Last sector')"
ROOT_FIRST="$(part_field 2 'First sector')"
ROOT_LAST="$(part_field 2 'Last sector')"

if [[ -z "${BOOT_FIRST}" || -z "${BOOT_LAST}" || -z "${ROOT_FIRST}" || -z "${ROOT_LAST}" ]]; then
    echo "error: could not inspect GPT partition offsets in ${DISK}" >&2
    exit 1
fi

BOOT_SECTORS=$((BOOT_LAST - BOOT_FIRST + 1))
ROOT_SECTORS=$((ROOT_LAST - ROOT_FIRST + 1))
ROOT_BYTES=$((((ROOT_SECTORS * 512) / 4096) * 4096))
BOOT_BYTES=$((BOOT_SECTORS * 512))
KERNEL_BYTES="$(ame_file_size "${KERNEL_BIN}")"
KERNEL_BLOCKS=$(((KERNEL_BYTES + 511) / 512))
KERNEL_LBA_HEX="$(printf '%x' "${BOOT_FIRST}")"
KERNEL_BLOCKS_HEX="$(printf '%x' "${KERNEL_BLOCKS}")"

if (( KERNEL_BYTES > BOOT_BYTES )); then
    echo "error: kernel image ${KERNEL_BIN} (${KERNEL_BYTES} bytes) exceeds raw boot partition (${BOOT_BYTES} bytes)" >&2
    exit 1
fi

BOOT_CMD="${BUILD_DIR}/boot.raw.cmd"
cat > "${BOOT_CMD}" <<'EOF_BOOT_CMD'
setenv kernel_addr_r 0x40080000
virtio scan
virtio dev 0
virtio read ${kernel_addr_r} __AME_KERNEL_LBA__ __AME_KERNEL_BLOCKS__
booti ${kernel_addr_r} - ${fdtcontroladdr}
EOF_BOOT_CMD
sed \
    -e "s/__AME_KERNEL_LBA__/${KERNEL_LBA_HEX}/g" \
    -e "s/__AME_KERNEL_BLOCKS__/${KERNEL_BLOCKS_HEX}/g" \
    "${BOOT_CMD}" > "${BOOT_CMD}.tmp"
mv "${BOOT_CMD}.tmp" "${BOOT_CMD}"

python3 "${ROOT}/scripts/mx/mkamefs.py" create \
    --output "${ROOT_IMG}" \
    --build-dir "${BUILD_DIR}" \
    --size "${ROOT_BYTES}"
dd if="${ROOT_IMG}" of="${DISK}" bs=512 seek="${ROOT_FIRST}" conv=notrunc
dd if="${KERNEL_BIN}" of="${DISK}" bs=512 seek="${BOOT_FIRST}" conv=notrunc

echo "built Ame disk image: ${DISK}"
echo "  raw boot partition: sectors ${BOOT_FIRST}-${BOOT_LAST}, kernel blocks=${KERNEL_BLOCKS} (0x${KERNEL_BLOCKS_HEX})"
echo "  root partition: sectors ${ROOT_FIRST}-${ROOT_LAST}"
echo "  U-Boot raw boot commands: ${BOOT_CMD}"
