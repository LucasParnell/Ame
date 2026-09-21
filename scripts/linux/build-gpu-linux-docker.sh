#!/usr/bin/env bash
#
# scripts/linux/build-gpu-linux-docker.sh — build the GPU appliance guest kernel
# inside a Linux container.
#
# Why a container: the kernel's host tools (scripts/sorttable.c) include <elf.h>,
# which macOS does not ship, and a native build therefore cannot complete on a
# Mac even with GNU userland shims. The container provides a normal Linux build
# environment, so scripts/linux/build-gpu-linux.sh runs unmodified and the
# minimal-config checker executes against the real resolved config.
#
# Env:
#   DANGO_LINUX_SRC         Linux source tree (default: $HOME/Code/linux)
#   DANGO_LINUX_OUT         artifact dir (default: <repo>/build/dango-linux-gpu)
#   DANGO_LINUX_BUILD_IMAGE container image (default: ubuntu:24.04)
set -euo pipefail

AME="$(cd "$(dirname "$0")/../.." && pwd)"
: "${DANGO_LINUX_SRC:=$HOME/Code/linux}"
: "${DANGO_LINUX_OUT:=${AME}/build/dango-linux-gpu}"
IMAGE="${DANGO_LINUX_BUILD_IMAGE:-ubuntu:24.04}"

command -v docker >/dev/null 2>&1 || {
    echo "error: docker not found (install colima + docker, then 'colima start')" >&2
    exit 1
}
docker info >/dev/null 2>&1 || {
    echo "error: docker daemon not reachable (run 'colima start')" >&2
    exit 1
}
[[ -f "${DANGO_LINUX_SRC}/Makefile" ]] || {
    echo "error: no Linux source at ${DANGO_LINUX_SRC}" >&2
    exit 1
}

mkdir -p "${DANGO_LINUX_OUT}"

# The tree is arm64, so an aarch64 container builds ARCH=arm64 natively — no
# cross toolchain required. Sources are read-only; generated objects persist in
# Ame's build directory for incremental builds, and final artifacts go to /out.
exec docker run --rm \
    --platform linux/arm64 \
    -e DANGO_LINUX_SRC=/linux \
    -e DANGO_GPU_KOUT=/ame/build/dango-linux-gpu-kout \
    -e DANGO_LINUX_OUT=/out \
    -e DEBIAN_FRONTEND=noninteractive \
    -v "${AME}:/ame" \
    -v "${DANGO_LINUX_SRC}:/linux:ro" \
    -v "${DANGO_LINUX_OUT}:/out" \
    "${IMAGE}" \
    bash -lc '
        set -euo pipefail
        apt-get update -qq
        apt-get install -y -qq --no-install-recommends \
            build-essential bc bison flex libelf-dev libssl-dev cpio kmod \
            file device-tree-compiler python3 ca-certificates >/dev/null
        cd /ame
        ./scripts/linux/build-gpu-linux.sh
    '
