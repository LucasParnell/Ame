#!/usr/bin/env bash
set -euo pipefail

AME="$(cd "$(dirname "$0")/../.." && pwd)"
source "${AME}/scripts/host.sh"
ame_host_tools
OUT="${DANGO_LINUX_OUT:-${AME}/build/dango-linux}"
TEMPLATE="${DANGO_DTS_TEMPLATE:-${AME}/linux-artifacts/dts/dango-linux-minimal.dts.in}"
GENERATED="${OUT}/$(basename "${TEMPLATE}" .dts.in).generated.dts"
DTB="${OUT}/${DANGO_DTB_NAME:-dango-linux.dtb}"
INITRD="${OUT}/initramfs.cpio"
INITRD_LOAD=$((0x46000000))
: "${DANGO_LINUX_BOOTARGS:=console=ttyAMA0,115200n8 earlycon=pl011,0x09000000 loglevel=4 nosmp maxcpus=1 cma=0 coherent_pool=1M cpuidle.off=1 rdinit=/init panic=-1}"

mkdir -p "${OUT}"

if [[ ! -f "${INITRD}" ]]; then
    echo "error: ${INITRD} missing; run scripts/linux/build-initramfs.sh first" >&2
    exit 1
fi

initrd_size=$(ame_file_size "${INITRD}")
# Keep the DTB range page-aligned so the eventual VM loader can copy/map it
# without needing byte-granular guest memory metadata.
initrd_end=$((INITRD_LOAD + ((initrd_size + 4095) & ~4095)))

fmt32() {
    printf '0x%08x' "$1"
}

escape_sed_replacement() {
    printf '%s' "$1" | sed -e 's/[\\&/]/\\&/g'
}

start_hi=$(( (INITRD_LOAD >> 32) & 0xffffffff ))
start_lo=$(( INITRD_LOAD & 0xffffffff ))
end_hi=$(( (initrd_end >> 32) & 0xffffffff ))
end_lo=$(( initrd_end & 0xffffffff ))

bootargs_escaped=$(escape_sed_replacement "${DANGO_LINUX_BOOTARGS}")

sed \
  -e "s/@BOOTARGS@/${bootargs_escaped}/g" \
  -e "s/@INITRD_START_HI@/$(fmt32 "${start_hi}")/g" \
  -e "s/@INITRD_START_LO@/$(fmt32 "${start_lo}")/g" \
  -e "s/@INITRD_END_HI@/$(fmt32 "${end_hi}")/g" \
  -e "s/@INITRD_END_LO@/$(fmt32 "${end_lo}")/g" \
  "${TEMPLATE}" > "${GENERATED}"

if command -v dtc >/dev/null 2>&1; then
    dtc -I dts -O dtb -o "${DTB}" "${GENERATED}"
elif [[ -n "${DANGO_LINUX_SRC:-}" && -x "${DANGO_LINUX_SRC}/scripts/dtc/dtc" ]]; then
    "${DANGO_LINUX_SRC}/scripts/dtc/dtc" -I dts -O dtb -o "${DTB}" "${GENERATED}"
else
    echo "error: dtc not found" >&2
    echo "Install dtc, or build Linux once so scripts/dtc/dtc exists." >&2
    exit 1
fi

test -s "${DTB}" || {
    echo "error: failed to create ${DTB}" >&2
    exit 1
}
