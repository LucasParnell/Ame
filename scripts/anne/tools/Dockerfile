# Linux host tools and AArch64 compiler for the Huawei GPU appliance.
FROM ubuntu:24.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
    clang llvm lld bc bison flex libelf-dev libssl-dev cpio kmod file \
    device-tree-compiler curl git ca-certificates meson ninja-build pkg-config \
    python3 python3-mako python3-yaml python3-packaging python3-ply \
    glslang-tools spirv-tools squashfs-tools xz-utils zstd patch \
    && rm -rf /var/lib/apt/lists/*
