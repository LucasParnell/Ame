#!/usr/bin/env bash
# CPU shader-compiler regression using Mesa's fake DRM device. No GPU proof.
set -euo pipefail
AME="$(cd "$(dirname "$0")/../.." && pwd)"
MESA_BUILD="${DANGO_MESA_BUILD:-${AME}/out/mesa-panfrost/mesa-build}"
GUEST="${DANGO_LINUX_OUT:-${AME}/out/dango-linux-panfrost}"
meson configure "${MESA_BUILD}" -Dtools=drm-shim
ninja -C "${MESA_BUILD}" src/panfrost/drm-shim/libpanfrost_noop_drm_shim.so
echo 'Checking the AArch64 Midgard shader compiler with emulated DRM; no GPU jobs execute.'
timeout 15 qemu-aarch64 -L "${GUEST}/rootfs" \
    -E "LD_PRELOAD=${MESA_BUILD}/src/panfrost/drm-shim/libpanfrost_noop_drm_shim.so" \
    -E PAN_GPU_ID=830 "${GUEST}/panfrost-renderer" --compile-only
