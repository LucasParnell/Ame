#!/usr/bin/env bash
#
# scripts/linux/build-vkd.sh — build dango-vkd and (optionally) stage Mesa lavapipe.
#
# dango-vkd is the guest half of the Phase N remote Vulkan transport. It reuses
# the existing Dango channel transport (linux-artifacts/gpud/gpud_channel.c) and
# links into the appliance init binary exactly like dango-gpud (see
# build-agent.sh, which adds its sources under DANGO_GPU_VKD=1).
#
# Two artifacts come out of this script:
#
#   1. The vkd objects / a freestanding vkd executable (the *default* build).
#      This is what the current no-libc initramfs can actually run: dango-vkd
#      serves the protocol and refuses submits with MX_ERR_SERVICE_UNAVAILABLE,
#      because the guest carries no Vulkan loader or ICD.
#
#   2. With VKD_STAGE_LAVAPIPE=1, the Mesa lavapipe ICD + the Vulkan loader are
#      copied out of the build container into ${OUT}/vulkan/. That is the raw
#      material for the *lit* build (VKD_HAVE_VULKAN=1). It is NOT sufficient on
#      its own: a dynamically loaded ICD needs a libc userspace (dynamic loader +
#      glibc) inside the guest, which the freestanding initramfs does not have.
#      The lit build therefore also needs a libc guest rootfs; this script stages
#      the Vulkan pieces and prints the remaining gap instead of pretending.
#
# Env:
#   DANGO_LINUX_OUT       artifact dir (default: <repo>/build/dango-linux-gpu)
#   VKD_CC                AArch64 Linux compiler (default: discovered)
#   VKD_HAVE_VULKAN       build the real-Vulkan executor (needs a libc sysroot)
#   VKD_SYSROOT           sysroot with libvulkan + the loader (lit build)
#   VKD_STAGE_LAVAPIPE    stage the ICD/loader artifacts into ${OUT}/vulkan
set -euo pipefail

AME="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${DANGO_LINUX_OUT:-${AME}/build/dango-linux-gpu}"
VKD_SRC="${AME}/linux-artifacts/vkd"
GPUD_CHANNEL="${AME}/linux-artifacts/gpud/gpud_channel.c"
HAVE_VULKAN="${VKD_HAVE_VULKAN:-0}"
STAGE="${VKD_STAGE_LAVAPIPE:-0}"
mkdir -p "${OUT}"

CFLAGS=(
    -std=c11 -Os -ffreestanding -fno-builtin -fno-stack-protector
    -Wall -Wextra -Werror
    -I"${AME}/shared/include"
    -I"${AME}/linux-artifacts/gpud"
    -I"${VKD_SRC}"
)

pick_cc() {
    if [[ -n "${VKD_CC:-}" ]]; then echo "${VKD_CC}"; return; fi
    if [[ -n "${CROSS_COMPILE:-}" ]] && command -v "${CROSS_COMPILE}gcc" >/dev/null 2>&1; then
        echo "${CROSS_COMPILE}gcc"; return
    fi
    if command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then echo aarch64-linux-gnu-gcc; return; fi
    # Compile-only objects, so the target triple alone is enough; the guest
    # agent link (build-agent.sh) owns -fuse-ld=lld.
    if command -v clang >/dev/null 2>&1; then echo "clang --target=aarch64-linux-gnu"; return; fi
    echo "" 
}
CC_SPEC="$(pick_cc)"
[[ -n "${CC_SPEC}" ]] || { echo "error: no AArch64 Linux compiler found" >&2; exit 1; }

# The executor define is the only difference between the fallback and the lit build.
if [[ "${HAVE_VULKAN}" == "1" ]]; then
    CFLAGS+=(-DVKD_HAVE_VULKAN=1)
    [[ -n "${VKD_SYSROOT:-}" ]] || {
        echo "error: VKD_HAVE_VULKAN=1 needs VKD_SYSROOT with libvulkan + <vulkan/vulkan.h>" >&2
        exit 1
    }
    CFLAGS+=(-I"${VKD_SYSROOT}/usr/include" -L"${VKD_SYSROOT}/usr/lib/aarch64-linux-gnu" -lvulkan)
fi

echo "building dango-vkd (VKD_HAVE_VULKAN=${HAVE_VULKAN}) with: ${CC_SPEC}"
# shellcheck disable=SC2086
${CC_SPEC} "${CFLAGS[@]}" -c "${VKD_SRC}/vkd_vulkan.c" -o "${OUT}/vkd_vulkan.o"
# shellcheck disable=SC2086
${CC_SPEC} "${CFLAGS[@]}" -c "${VKD_SRC}/vkd_main.c" -o "${OUT}/vkd_main.o"
# shellcheck disable=SC2086
${CC_SPEC} "${CFLAGS[@]}" -c "${GPUD_CHANNEL}" -o "${OUT}/vkd_channel.o"
echo "dango-vkd objects written to ${OUT}"

if [[ "${STAGE}" == "1" ]]; then
    mkdir -p "${OUT}/vulkan"
    # The container build (build-vkd-docker.sh) populates ${OUT}/vulkan with the
    # extracted ICD/loader before calling us; here we only verify and report.
    if compgen -G "${OUT}/vulkan/*" >/dev/null; then
        echo "staged Vulkan artifacts:"
        ls -1 "${OUT}/vulkan"
    else
        echo "note: VKD_STAGE_LAVAPIPE=1 but ${OUT}/vulkan is empty (run build-vkd-docker.sh)"
    fi
    cat >&2 <<'GAP'
--------------------------------------------------------------------
dango-vkd lit build gap (not hidden):
  * The staged ICD/loader come from a glibc userspace. The Ame guest is a
    freestanding, no-libc initramfs, so there is nothing to dlopen() them.
  * A lit dango-vkd therefore also needs a libc guest rootfs (dynamic loader +
    glibc) in the initramfs, plus the kernel config to run it. That is the
    remaining work for the end-to-end lavapipe appliance.
  * Until then dango-vkd runs its fallback executor: protocol served, submits
    refused with MX_ERR_SERVICE_UNAVAILABLE, native clients keep their local path.
--------------------------------------------------------------------
GAP
fi
