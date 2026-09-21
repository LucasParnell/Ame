#!/usr/bin/env bash
# macOS/Linux arm64 builder for the libc lavapipe executor and its guest rootfs.
set -euo pipefail
AME="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${DANGO_LINUX_OUT:-${AME}/build/dango-vkd}"
IMAGE="${DANGO_VKD_BUILD_IMAGE:-ame-vkd-build:local}"
command -v docker >/dev/null 2>&1 || { echo "error: install docker and start colima" >&2; exit 1; }
docker info >/dev/null 2>&1 || { echo "error: docker daemon unavailable; start colima" >&2; exit 1; }
mkdir -p "${OUT}"
OUT="$(cd "${OUT}" && pwd)"
docker build --platform linux/arm64 --build-arg "BASE_IMAGE=${DANGO_LINUX_BUILD_IMAGE:-ubuntu:24.04}" \
    -t "${IMAGE}" -f "${AME}/linux-artifacts/vkd/Dockerfile" "${AME}/linux-artifacts/vkd"
exec docker run --rm --platform linux/arm64 --network none \
    -e DANGO_LINUX_OUT=/out -v "${AME}:/ame:ro" -v "${OUT}:/out" \
    "${IMAGE}" /ame/scripts/linux/build-vkd.sh
