#!/usr/bin/env bash
# Build a local U-Boot firmware for QEMU virt/aarch64.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${AME_BUILD_DIR:-${ROOT}/build}"
UBOOT_TAG="${UBOOT_TAG:-v2026.04}"
SRC_DIR="${UBOOT_SRC_DIR:-${BUILD_DIR}/u-boot-${UBOOT_TAG}}"
OUT="${UBOOT_OUT:-${BUILD_DIR}/u-boot-qemu-arm64.bin}"
WRAP_DIR="${BUILD_DIR}/u-boot-toolchain-wrap"
RAW_BOOTCOMMAND='setenv kernel_addr_r 0x40080000; cp.b ${fdtcontroladdr} 0x50000000 0x20000; virtio scan; virtio dev 0; virtio read ${kernel_addr_r} 800 20000; booti ${kernel_addr_r} - 0x50000000'

need() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "error: required tool not found: $1" >&2
        exit 1
    fi
}

need git
need make
need bison
need flex
need dtc
need python3
need openssl

mkdir -p "${BUILD_DIR}"

TOOL_PATH="${PATH}"
if ! command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
    need clang
    need ld.lld
    rm -rf "${WRAP_DIR}"
    mkdir -p "${WRAP_DIR}"

    cat > "${WRAP_DIR}/aarch64-linux-gnu-gcc" <<'EOF_WRAP'
#!/usr/bin/env bash
exec clang --target=aarch64-linux-gnu "$@"
EOF_WRAP
    cat > "${WRAP_DIR}/aarch64-linux-gnu-cc" <<'EOF_WRAP'
#!/usr/bin/env bash
exec clang --target=aarch64-linux-gnu "$@"
EOF_WRAP
    cat > "${WRAP_DIR}/aarch64-linux-gnu-as" <<'EOF_WRAP'
#!/usr/bin/env bash
exec clang --target=aarch64-linux-gnu -c "$@"
EOF_WRAP
    cat > "${WRAP_DIR}/aarch64-linux-gnu-ld" <<'EOF_WRAP'
#!/usr/bin/env bash
exec ld.lld "$@"
EOF_WRAP
    cat > "${WRAP_DIR}/aarch64-linux-gnu-ar" <<'EOF_WRAP'
#!/usr/bin/env bash
exec llvm-ar "$@"
EOF_WRAP
    cat > "${WRAP_DIR}/aarch64-linux-gnu-nm" <<'EOF_WRAP'
#!/usr/bin/env bash
exec llvm-nm "$@"
EOF_WRAP
    cat > "${WRAP_DIR}/aarch64-linux-gnu-objcopy" <<'EOF_WRAP'
#!/usr/bin/env bash
exec llvm-objcopy "$@"
EOF_WRAP
    cat > "${WRAP_DIR}/aarch64-linux-gnu-objdump" <<'EOF_WRAP'
#!/usr/bin/env bash
exec llvm-objdump "$@"
EOF_WRAP
    cat > "${WRAP_DIR}/aarch64-linux-gnu-readelf" <<'EOF_WRAP'
#!/usr/bin/env bash
exec llvm-readelf "$@"
EOF_WRAP
    cat > "${WRAP_DIR}/aarch64-linux-gnu-strip" <<'EOF_WRAP'
#!/usr/bin/env bash
exec llvm-strip "$@"
EOF_WRAP
    cat > "${WRAP_DIR}/aarch64-linux-gnu-gcc-ar" <<'EOF_WRAP'
#!/usr/bin/env bash
exec llvm-ar "$@"
EOF_WRAP
    cat > "${WRAP_DIR}/aarch64-linux-gnu-gcc-nm" <<'EOF_WRAP'
#!/usr/bin/env bash
exec llvm-nm "$@"
EOF_WRAP
    chmod +x "${WRAP_DIR}"/aarch64-linux-gnu-*
    TOOL_PATH="${WRAP_DIR}:${PATH}"
    echo "warning: aarch64-linux-gnu-gcc not found; using clang wrapper fallback" >&2
fi

if [[ ! -d "${SRC_DIR}/.git" ]]; then
    rm -rf "${SRC_DIR}"
    git clone --depth 1 --branch "${UBOOT_TAG}" \
        https://source.denx.de/u-boot/u-boot.git \
        "${SRC_DIR}"
fi

PATH="${TOOL_PATH}:${ROOT}/scripts/linux/toolbin" \
make -C "${SRC_DIR}" CROSS_COMPILE=aarch64-linux-gnu- qemu_arm64_defconfig

"${SRC_DIR}/scripts/config" --file "${SRC_DIR}/.config" \
    --disable BOOTSTD_BOOTCOMMAND \
    --enable USE_BOOTCOMMAND \
    --set-val BOOTDELAY 0 \
    --set-str BOOTCOMMAND "${RAW_BOOTCOMMAND}"

PATH="${TOOL_PATH}:${ROOT}/scripts/linux/toolbin" \
make -C "${SRC_DIR}" -j"$(nproc)" CROSS_COMPILE=aarch64-linux-gnu-

cp "${SRC_DIR}/u-boot.bin" "${OUT}"
echo "built U-Boot: ${OUT}"
