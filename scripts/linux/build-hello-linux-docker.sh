#!/usr/bin/env bash
# Linux host tools need libelf/GNU userland. Keep the source read-only and all
# generated artifacts in the workspace, as in the existing GPU build workflow.
set -euo pipefail

AME="$(cd "$(dirname "$0")/../.." && pwd)"
: "${DANGO_LINUX_SRC:=$HOME/Code/linux}"
: "${DANGO_LINUX_OUT:=${AME}/build/dango-linux}"
IMAGE="${DANGO_LINUX_BUILD_IMAGE:-ubuntu:24.04}"

docker info >/dev/null 2>&1 || {
    echo "error: Docker is not reachable; start the local container runtime" >&2
    exit 1
}
[[ -f "${DANGO_LINUX_SRC}/Makefile" ]] || {
    echo "error: no pinned Linux source at ${DANGO_LINUX_SRC}" >&2
    exit 1
}
mkdir -p "${DANGO_LINUX_OUT}"
exec docker run --rm --platform linux/arm64 \
    -e DANGO_LINUX_SRC=/linux \
    -e DANGO_LINUX_OUT=/out \
    -e DANGO_LINUX_KOUT=/ame/build/dango-linux-hello-kout \
    -e DANGO_LINUX_JOBS="${DANGO_LINUX_JOBS:-4}" \
    -e DANGO_LINUX_TIMER_HZ="${DANGO_LINUX_TIMER_HZ:-0}" \
    -e DEBIAN_FRONTEND=noninteractive \
    -v "${AME}:/ame" \
    -v "${DANGO_LINUX_SRC}:/linux:ro" \
    -v "${DANGO_LINUX_OUT}:/out" \
    "${IMAGE}" bash -lc '
        set -euo pipefail
        apt-get update -qq
        apt-get install -y -qq --no-install-recommends \
            build-essential clang lld llvm bc bison flex libelf-dev libssl-dev \
            cpio file device-tree-compiler python3 git ca-certificates >/dev/null
        cd /ame
        ./scripts/linux/build-min-linux.sh
    '
