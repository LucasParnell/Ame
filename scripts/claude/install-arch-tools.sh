#!/usr/bin/env bash
set -euo pipefail
sudo pacman -Syu --needed \
  base-devel cmake ninja clang llvm lld qemu-system-aarch64 \
  ripgrep fd jq python nodejs npm uv rust git
