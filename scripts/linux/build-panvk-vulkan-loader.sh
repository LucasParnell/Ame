#!/usr/bin/env bash
# Standard Khronos loader for CTS in the isolated AArch64 GPU guest.
set -euo pipefail
AME="$(cd "$(dirname "$0")/../.." && pwd)"
SOURCE="${AME}/out/vulkan-loader-midgard"
HEADERS="${AME}/out/vulkan-headers-midgard"
OUT="${AME}/out/vulkan-loader-midgard-build"
PREFIX="${AME}/out/vulkan-loader-midgard-install"
HEADER_PREFIX="${AME}/out/vulkan-headers-midgard-install"
[[ -d "${SOURCE}/.git" ]] || git clone --depth 1 --branch vulkan-sdk-1.3.296.0 https://github.com/KhronosGroup/Vulkan-Loader.git "${SOURCE}"
[[ -d "${HEADERS}/.git" ]] || git clone --depth 1 --branch vulkan-sdk-1.3.296.0 https://github.com/KhronosGroup/Vulkan-Headers.git "${HEADERS}"
# Resume a clone interrupted by the host reboot without discarding files.
if ! git -C "${HEADERS}" rev-parse --verify HEAD >/dev/null 2>&1; then
    git -C "${HEADERS}" fetch --depth 1 origin refs/tags/vulkan-sdk-1.3.296.0
    git -C "${HEADERS}" checkout --detach FETCH_HEAD
fi
[[ "$(git -C "${SOURCE}" rev-parse HEAD)" == 7aeb5e4324957b6f60dac5c8cedb955df4fecbfb ]]
[[ "$(git -C "${HEADERS}" rev-parse HEAD)" == 29f979ee5aa58b7b005f805ea8df7a855c39ff37 ]]
cmake -S "${HEADERS}" -B "${AME}/out/vulkan-headers-midgard-build" -DCMAKE_INSTALL_PREFIX="${HEADER_PREFIX}" -DVULKAN_HEADERS_ENABLE_MODULE=OFF
cmake --install "${AME}/out/vulkan-headers-midgard-build"
mkdir -p "${OUT}"
cat > "${OUT}/aarch64-toolchain.cmake" <<'CMAKE'
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
CMAKE
cmake -S "${SOURCE}" -B "${OUT}" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${OUT}/aarch64-toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="${PREFIX}" -DCMAKE_INSTALL_LIBDIR=lib \
    -DVULKAN_HEADERS_INSTALL_DIR="${HEADER_PREFIX}" -DUPDATE_DEPS=OFF \
    -DBUILD_WSI_XCB_SUPPORT=OFF -DBUILD_WSI_XLIB_SUPPORT=OFF \
    -DBUILD_WSI_WAYLAND_SUPPORT=OFF -DBUILD_TESTS=OFF
cmake --build "${OUT}" --target vulkan -j1
cmake --install "${OUT}"
echo "Vulkan loader: ${PREFIX}/lib/libvulkan.so.1"
