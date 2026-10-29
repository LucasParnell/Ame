#!/usr/bin/env bash
# Compiler/descriptor checks only. DRM shim does not execute GPU commands.
set -euo pipefail
ulimit -c 0
AME="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${DANGO_PANVK_BUILD:-${AME}/out/mesa-panvk-midgard}"
CLIENT="${DANGO_PANVK_RENDERER_BUILD:-${AME}/out/panvk-renderer}"
"${AME}/scripts/linux/build-panvk-renderer.sh"
aarch64-linux-gnu-gcc -std=c11 -O2 -g -Wall -Wextra -Werror \
    -I"${BUILD}/mesa-24.3.4/include" \
    "${AME}/tests/panvk_command_buffer_probe.c" -ldl \
    -o "${CLIENT}/panvk-command-buffer-probe"
timeout 30 qemu-aarch64 -L /usr/aarch64-linux-gnu \
    -E "LD_LIBRARY_PATH=${BUILD}/build/src/panfrost/vulkan:${AME}/out/mesa-panfrost/install/lib" \
    -E "LD_PRELOAD=${BUILD}/build/src/panfrost/drm-shim/libpanfrost_noop_drm_shim.so" \
    -E PAN_GPU_ID=830 -E PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1 \
    -E PANVK_MIDGARD_EXPERIMENTAL=1 "${CLIENT}/panvk-command-buffer-probe"
for mode in --compile-only --record-only --record-load-only; do
    timeout 30 qemu-aarch64 -L /usr/aarch64-linux-gnu \
        -E "LD_LIBRARY_PATH=${BUILD}/build/src/panfrost/vulkan:${AME}/out/mesa-panfrost/install/lib" \
        -E "LD_PRELOAD=${BUILD}/build/src/panfrost/drm-shim/libpanfrost_noop_drm_shim.so" \
        -E PAN_GPU_ID=830 -E PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1 \
        -E PANVK_MIDGARD_EXPERIMENTAL=1 "${CLIENT}/panvk-triangle" "${mode}"
done
# Compile and record the matrix, retaining final NIR/MIR to check that the
# overflow-index guard survives compilation without a wide scalar compare.
GUARD_DUMP="${BUILD}/index-guard-recording.log"
env MIDGARD_MESA_DEBUG=shaders,internal timeout 30 qemu-aarch64 -L /usr/aarch64-linux-gnu \
    -E "LD_LIBRARY_PATH=${BUILD}/build/src/panfrost/vulkan:${AME}/out/mesa-panfrost/install/lib" \
    -E "LD_PRELOAD=${BUILD}/build/src/panfrost/drm-shim/libpanfrost_noop_drm_shim.so" \
    -E PAN_GPU_ID=830 -E PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1 \
    -E PANVK_MIDGARD_EXPERIMENTAL=1 "${CLIENT}/panvk-suite" --record-only >"${GUARD_DUMP}" 2>&1 || {
        tail -80 "${GUARD_DUMP}"
        exit 1
    }
python3 "${AME}/scripts/linux/check-midgard-index-guards.py" "${GUARD_DUMP}"
tail -1 "${GUARD_DUMP}"
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
echo 'Compiler, command recording, preload/sample/compute matrix and epoch-timestamp checks passed. These do not execute GPU work.'
