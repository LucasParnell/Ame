#!/usr/bin/env bash
#
# scripts/linux/build-gpu-linux.sh — build the Linux GPU appliance guest kernel.
#
# Same shape as build-min-linux.sh, but merges the GPU fragment on top of the
# minimal hello fragment and enforces the minimal-configuration policy
# (docs/LINUX_APPLIANCE_MINIMAL.md) on the resolved .config before building.
#
# Env:
#   DANGO_LINUX_SRC   Linux source tree (default: $HOME/Code/linux)
#   DANGO_LINUX_TAG   tag to clone when missing (default: v6.12)
#   DANGO_GPU_KOUT    kernel build dir (default: <src>/out-dango-gpu)
#   DANGO_LINUX_OUT   artifact dir (default: build/dango-linux-gpu)
#   MAKE              make to use (default: a GNU make >= 4 on PATH)
set -euo pipefail

AME="$(cd "$(dirname "$0")/../.." && pwd)"
: "${DANGO_LINUX_SRC:=$HOME/Code/linux}"
: "${DANGO_LINUX_TAG:=v6.12}"
KOUT="${DANGO_GPU_KOUT:-${DANGO_LINUX_SRC}/out-dango-gpu}"
OUT="${DANGO_LINUX_OUT:-${AME}/build/dango-linux-gpu}"
FRAG_HELLO="${AME}/linux-artifacts/config/dango_hello_arm64.fragment"
FRAG_GPU="${AME}/linux-artifacts/config/dango_gpu_arm64.fragment"
CHECK="${AME}/scripts/linux/check-minimal-config.py"

# GNU make >= 4 is required; macOS ships 3.81 as /usr/bin/make.
MAKE="${MAKE:-}"
if [[ -z "${MAKE}" ]]; then
    for cand in make gmake /opt/homebrew/bin/make; do
        if command -v "${cand}" >/dev/null 2>&1; then
            ver="$("${cand}" --version | head -1 | awk '{print $3}')"
            if [[ "${ver%%.*}" -ge 4 ]]; then MAKE="${cand}"; break; fi
        fi
    done
fi
[[ -n "${MAKE}" ]] || { echo "error: GNU make >= 4 required (brew install make)" >&2; exit 1; }

if [[ ! -f "${DANGO_LINUX_SRC}/Makefile" ]]; then
    echo "error: no Linux source at ${DANGO_LINUX_SRC} (set DANGO_LINUX_SRC)" >&2
    exit 1
fi

# Homebrew LLVM provides clang/lld/llvm-* for `make LLVM=1`.
if [[ -d /opt/homebrew/opt/llvm/bin ]]; then
    PATH="/opt/homebrew/opt/llvm/bin:${PATH}"
fi

# macOS ships BSD userland; the kernel's build/config scripts assume GNU tools
# (merge_config.sh uses `readlink -m` and GNU sed). Homebrew provides GNU
# equivalents as g<tool>; expose them under their plain names for this build.
shim="$(mktemp -d)"
shim_tool() { # <name> <gnu-binary>
    if command -v "$2" >/dev/null 2>&1; then
        printf '#!/bin/sh\nexec %s "$@"\n' "$2" > "${shim}/$1"
        chmod +x "${shim}/$1"
    fi
}
shim_tool readlink greadlink
shim_tool sed      gsed
shim_tool awk      gawk
shim_tool install  ginstall
shim_tool cp       gcp
PATH="${shim}:${PATH}"
export PATH
trap 'rm -rf "${shim}"' EXIT

mkdir -p "${OUT}" "${KOUT}"

stamp="${KOUT}/.dango-gpu-configured"
if [[ ! -f "${stamp}" || "${FRAG_GPU}" -nt "${stamp}" || "${FRAG_HELLO}" -nt "${stamp}" ]]; then
    rm -f "${stamp}"
    MAKE_ARGS=(ARCH=arm64)
if command -v clang >/dev/null 2>&1 && command -v ld.lld >/dev/null 2>&1; then
    MAKE_ARGS+=(LLVM=1)
elif [[ "${CROSS_COMPILE:-}" == "" && "$(uname -m)" != "aarch64" ]]; then
    echo "error: need clang+lld (LLVM=1) or a native aarch64 host / CROSS_COMPILE" >&2
    exit 1
fi

"${MAKE}" "${MAKE_ARGS[@]}" O="${KOUT}" -C "${DANGO_LINUX_SRC}" defconfig
    # Kernel-sanctioned fragment merge (-m: write the merged file, no make; the
    # caller runs olddefconfig). It rewrites duplicates rather than appending,
    # which is required because kconfig keeps the first assignment.
    "${DANGO_LINUX_SRC}/scripts/kconfig/merge_config.sh" -m -O "${KOUT}" \
        "${KOUT}/.config" "${FRAG_HELLO}" "${FRAG_GPU}"
    "${MAKE}" "${MAKE_ARGS[@]}" O="${KOUT}" -C "${DANGO_LINUX_SRC}" olddefconfig
    "${CHECK}" --config "${KOUT}/.config" --flow gpu
    touch "${stamp}"
else
    "${CHECK}" --config "${KOUT}/.config" --flow gpu
fi

"${MAKE}" "${MAKE_ARGS[@]}" O="${KOUT}" -C "${DANGO_LINUX_SRC}" -j"$(sysctl -n hw.logicalcpu 2>/dev/null || nproc)" Image

cp "${KOUT}/arch/arm64/boot/Image" "${OUT}/Image"
cp "${KOUT}/vmlinux" "${OUT}/vmlinux" 2>/dev/null || true
cp "${KOUT}/System.map" "${OUT}/System.map" 2>/dev/null || true
echo "built Linux GPU appliance kernel: ${OUT}/Image"
