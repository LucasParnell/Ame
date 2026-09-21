#!/usr/bin/env bash
# Build a libc guest executable, execute its Vulkan regression, and package the
# exact ELF dependency closure. macOS delegates to the arm64 Linux container.
# Outputs are separate from the 128 MiB freestanding GPU artifact profile.
set -euo pipefail
AME="$(cd "$(dirname "$0")/../.." && pwd)"
if [[ "$(uname -s)" != Linux || "$(uname -m)" != aarch64 || "$(id -u)" != 0 ]]; then
    exec "${AME}/scripts/linux/build-vkd-docker.sh"
fi
OUT="${DANGO_LINUX_OUT:-${AME}/build/dango-vkd}"
CC="${VKD_CC:-clang}"
mkdir -p "${OUT}"
CFLAGS=(-std=c11 -O2 -Wall -Wextra -Werror -fstack-protector-strong -D_FORTIFY_SOURCE=2
    -DVKD_HAVE_VULKAN=1 -I"${AME}/shared/include" -I"${AME}/linux-artifacts/gpud"
    -I"${AME}/linux-artifacts/vkd" -Wl,-z,relro,-z,now,-z,noexecstack)
"${CC}" "${CFLAGS[@]}" "${AME}/linux-artifacts/vkd/vkd_entry.c" \
    "${AME}/linux-artifacts/vkd/vkd_main.c" "${AME}/linux-artifacts/vkd/vkd_vulkan.c" "${AME}/linux-artifacts/vkd/vkd_runtime.c" \
    "${AME}/linux-artifacts/gpud/gpud_channel.c" -lvulkan -o "${OUT}/dango-vkd"
"${CC}" "${CFLAGS[@]}" "${AME}/tests/vkd_vulkan_probe.c" \
    "${AME}/linux-artifacts/vkd/vkd_vulkan.c" "${AME}/linux-artifacts/vkd/vkd_runtime.c" -lvulkan -o "${OUT}/vkd-probe"
# Validation layers stay in the build container, outside the shipped rootfs.
ICD=$(python3 -c 'import glob; files=glob.glob("/usr/share/vulkan/icd.d/lvp_icd*.json"); assert len(files)==1; print(files[0])')
VK_DRIVER_FILES="${ICD}" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation \
    "${OUT}/vkd-probe" > "${OUT}/validation.log" 2>&1
cat "${OUT}/validation.log"
if grep -Eq 'Validation Error|VUID-|TEST FAILED' "${OUT}/validation.log"; then
    echo "error: Vulkan validation failed" >&2
    exit 1
fi
python3 "${AME}/scripts/linux/stage-vkd-rootfs.py" "${OUT}"
# Both missing-ICD cleanup and real device execution run in the staged filesystem.
chroot "${OUT}/rootfs" /usr/bin/vkd-probe --missing-icd
chroot "${OUT}/rootfs" /usr/bin/vkd-probe
python3 "${AME}/scripts/linux/mkinitramfs.py" "${OUT}/dango-vkd" "${OUT}/initramfs.cpio.gz" --rootfs "${OUT}/rootfs"
python3 "${AME}/scripts/linux/check-initramfs.py" "${OUT}/initramfs.cpio.gz"
echo "Vulkan rootfs ready: ${OUT}/initramfs.cpio.gz (standalone Linux acceptance; see docs/GPU_APPLIANCE_REMAINING.md)"
