#!/usr/bin/env bash
set -euo pipefail
AME="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${DANGO_LINUX_OUT:-${AME}/out/dango-linux-panfrost}"
MESA="${DANGO_MESA_PREFIX:-${AME}/out/mesa-panfrost/install}"
[[ -f "${MESA}/lib/libEGL.so" ]] || { echo 'Build Mesa with scripts/linux/build-panfrost-mesa.sh first' >&2; exit 1; }
mkdir -p "${OUT}"
PANVK_ARGS=()
PANVK_STAGE_ARGS=()
if [[ -n "${DANGO_PANVK_PREFIX:-}" ]]; then
    [[ -f "${DANGO_PANVK_PREFIX}/lib/libvulkan_panfrost.so" ]] || {
        echo 'Build scripts/linux/build-panvk-mesa.sh first' >&2; exit 1;
    }
    VK_RPC_OUT="${OUT}/vulkan-rpc"
    python3 "${AME}/scripts/vulkan/generate-frontend.py" --output "${VK_RPC_OUT}"
    for VK_RPC_UNIT in vk-rpc-metadata vulkan_rpc; do
        VK_RPC_SOURCE="${VK_RPC_OUT}/${VK_RPC_UNIT}.cc"
        [[ "$VK_RPC_UNIT" == vulkan_rpc ]] && VK_RPC_SOURCE="${AME}/linux-artifacts/panfrost-renderer/vulkan_rpc.cc"
        aarch64-linux-gnu-g++ -std=c++20 -O2 -g -mcpu=cortex-a53 -Wall -Wextra -Werror \
            -fno-exceptions -fno-rtti -fno-threadsafe-statics \
            -I"${AME}/third_party/vulkan/include" -I"${AME}/shared/include" -I"${VK_RPC_OUT}" \
            -c "${VK_RPC_SOURCE}" -o "${VK_RPC_OUT}/${VK_RPC_UNIT}.o"
    done
    PANVK_ARGS=("${VK_RPC_OUT}/vk-rpc-metadata.o" "${VK_RPC_OUT}/vulkan_rpc.o" -DAME_ENABLE_PANVK=1 -ldl)
    if [[ "${DANGO_PANVK_SUITE_TRACE:-0}" == 1 ]]; then
        PANVK_ARGS+=(-DAME_PANVK_SUITE_TRACE=1)
    fi
    PANVK_STAGE_ARGS=(--extra-library "${DANGO_PANVK_PREFIX}/lib/libvulkan_panfrost.so")
fi
if [[ -n "${DANGO_PANVK_CTS_BINARY:-}" ]]; then
    [[ -n "${DANGO_PANVK_PREFIX:-}" ]] || { echo 'CTS requires the PanVK ICD' >&2; exit 1; }
    VK_LOADER="${DANGO_PANVK_VULKAN_LOADER:-${AME}/out/vulkan-loader-midgard-install/lib/libvulkan.so.1}"
    [[ -f "${VK_LOADER}" ]] || { echo 'Build the Vulkan loader first' >&2; exit 1; }
    cat > "${OUT}/panvk-cts-icd.json" <<'ICD'
{"file_format_version":"1.0.0","ICD":{"library_path":"/usr/lib/libvulkan_panfrost.so","api_version":"1.0.0"}}
ICD
    PANVK_STAGE_ARGS+=(--extra-executable "${DANGO_PANVK_CTS_BINARY}" --extra-library "${VK_LOADER}"
        --icd-json "${OUT}/panvk-cts-icd.json")
    if [[ -n "${DANGO_PANVK_CTS_DATA:-}" ]]; then PANVK_STAGE_ARGS+=(--cts-data "${DANGO_PANVK_CTS_DATA}"); fi
fi
aarch64-linux-gnu-gcc -std=c11 -O3 -g -mcpu=cortex-a53 -Wall -Wextra -Werror \
    -I"${AME}/shared/include" -I"${MESA}/include" -I"${MESA}/include/libdrm" \
    -I/usr/aarch64-linux-gnu/include/drm \
    "${AME}/linux-artifacts/panfrost-renderer/main.c" \
    -L"${MESA}/lib" -Wl,-rpath-link,"${MESA}/lib" -Wl,--disable-new-dtags,-rpath,/usr/lib \
    "${PANVK_ARGS[@]}" -lEGL -lGLESv2 -ldrm -o "${OUT}/panfrost-renderer"
python3 "${AME}/scripts/linux/stage-panfrost-rootfs.py" \
    --agent "${OUT}/panfrost-renderer" --mesa "${MESA}" --output "${OUT}" "${PANVK_STAGE_ARGS[@]}"
case "${DANGO_PANFROST_ROOTFS:-initramfs}" in
    initramfs)
        python3 "${AME}/scripts/linux/mkinitramfs.py" "${OUT}/dango-agent" \
            "${OUT}/initramfs.cpio.gz" --rootfs "${OUT}/rootfs"
        # Linux recognizes the compression magic; the artifact filename is ABI-stable.
        mv "${OUT}/initramfs.cpio.gz" "${OUT}/initramfs.cpio"
        rm -f "${OUT}/rootfs-storage.json" ;;
    squashfs)
        python3 "${AME}/scripts/linux/build-panfrost-squashfs.py" --output "${OUT}" ;;
    *) echo 'error: DANGO_PANFROST_ROOTFS must be initramfs or squashfs' >&2; exit 1 ;;
esac
python3 "${AME}/scripts/linux/check-initramfs.py" "${OUT}/initramfs.cpio"
# Renderer-only iteration can change the compressed archive length. Preserve
# hardware/timer bindings while refreshing the initrd bound in an existing DTB.
if [[ -f "${OUT}/dango-linux.dtb" ]]; then
    python3 "${AME}/scripts/linux/update-initrd-dtb.py" "${OUT}"
fi
