#!/usr/bin/env bash
# Compiler/descriptor checks only. DRM shim does not execute GPU commands.
set -euo pipefail
AME="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${DANGO_PANVK_BUILD:-${AME}/out/mesa-panvk-midgard}"
CLIENT="${DANGO_PANVK_RENDERER_BUILD:-${AME}/out/panvk-renderer}"
"${AME}/scripts/linux/build-panvk-renderer.sh"
for mode in --compile-only --record-only; do
    timeout 30 qemu-aarch64 -L /usr/aarch64-linux-gnu \
        -E "LD_LIBRARY_PATH=${BUILD}/build/src/panfrost/vulkan:${AME}/out/mesa-panfrost/install/lib" \
        -E "LD_PRELOAD=${BUILD}/build/src/panfrost/drm-shim/libpanfrost_noop_drm_shim.so" \
        -E PAN_GPU_ID=830 -E PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1 \
        -E PANVK_MIDGARD_EXPERIMENTAL=1 "${CLIENT}/panvk-triangle" "${mode}"
done
# Match deterministic initramfs timestamps; enumeration must not depend on mtime.
EPOCH_ICD="$(mktemp -d "${BUILD}/epoch-icd.XXXXXX")"
trap 'rm -rf -- "$EPOCH_ICD"' EXIT
cp "${BUILD}/build/src/panfrost/vulkan/libvulkan_panfrost.so" "${EPOCH_ICD}/"
touch -d @0 "${EPOCH_ICD}/libvulkan_panfrost.so"
timeout 30 qemu-aarch64 -L /usr/aarch64-linux-gnu \
    -E "LD_LIBRARY_PATH=${EPOCH_ICD}:${AME}/out/mesa-panfrost/install/lib" \
    -E "LD_PRELOAD=${BUILD}/build/src/panfrost/drm-shim/libpanfrost_noop_drm_shim.so" \
    -E PAN_GPU_ID=830 -E PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1 \
    -E PANVK_MIDGARD_EXPERIMENTAL=1 "${CLIENT}/panvk-triangle" --record-only
echo 'Compiler, command recording and epoch-timestamp checks passed. These do not execute GPU work.' 
