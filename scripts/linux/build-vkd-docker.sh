#!/usr/bin/env bash
#
# scripts/linux/build-vkd-docker.sh — stage Mesa lavapipe + the Vulkan loader for
# dango-vkd inside a Linux container, then build the vkd objects.
#
# Same shape as build-gpu-linux-docker.sh: the tree is arm64, so an aarch64
# container matches the guest ABI and no cross toolchain is needed. Ubuntu 24.04
# ships Mesa lavapipe (`mesa-vulkan-drivers` -> libvulkan_lvp.so + the ICD json)
# and the Khronos loader (`libvulkan1`), so we stage the distro packages rather
# than build Mesa from source.
#
# Env:
#   DANGO_LINUX_OUT         artifact dir (default: <repo>/build/dango-linux-gpu)
#   DANGO_LINUX_BUILD_IMAGE container image (default: ubuntu:24.04)
set -euo pipefail

AME="$(cd "$(dirname "$0")/../.." && pwd)"
: "${DANGO_LINUX_OUT:=${AME}/build/dango-linux-gpu}"
IMAGE="${DANGO_LINUX_BUILD_IMAGE:-ubuntu:24.04}"

command -v docker >/dev/null 2>&1 || { echo "error: docker not found" >&2; exit 1; }
docker info >/dev/null 2>&1 || { echo "error: docker daemon not reachable" >&2; exit 1; }

mkdir -p "${DANGO_LINUX_OUT}/vulkan"

exec docker run --rm \
    --platform linux/arm64 \
    -e DANGO_LINUX_OUT=/out \
    -e VKD_SYSROOT=/sysroot \
    -e VKD_STAGE_LAVAPIPE=1 \
    -e DEBIAN_FRONTEND=noninteractive \
    -v "${AME}:/ame" \
    -v "${DANGO_LINUX_OUT}:/out" \
    "${IMAGE}" \
    bash -lc '
        set -euo pipefail
        apt-get update -qq
        apt-get install -y -qq --no-install-recommends \
            clang lld libc6-dev mesa-vulkan-drivers libvulkan1 libvulkan-dev \
            file ca-certificates >/dev/null
        echo "== lavapipe packages present =="
        dpkg -L mesa-vulkan-drivers | grep -E "lvp|icd" || true
        dpkg -L libvulkan1 | grep -E "libvulkan\.so" || true
        # Stage the lavapipe ICD json + the lavapipe .so + the Khronos loader
        # into /out/vulkan. Precise names only: a loose match also copies
        # unrelated files (e.g. clang intrinsic headers).
        cp -Lv /usr/share/vulkan/icd.d/lvp_icd.json /out/vulkan/
        cp -Lv /usr/lib/aarch64-linux-gnu/libvulkan_lvp.so /out/vulkan/
        cp -Lv /usr/lib/aarch64-linux-gnu/libvulkan.so.1 /out/vulkan/
        cp -Lv /usr/lib/aarch64-linux-gnu/libvulkan.so.1.3.275 /out/vulkan/ 2>/dev/null || true
        # A sysroot so build-vkd.sh can find <vulkan/vulkan.h> + libvulkan.
        mkdir -p /sysroot
        cp -a /usr/include/vulkan /sysroot/ 2>/dev/null || true
        cp -a /usr/lib/aarch64-linux-gnu/libvulkan.so* /sysroot/ 2>/dev/null || true
        cd /ame
        ./scripts/linux/build-vkd.sh
    '
