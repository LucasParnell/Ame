#!/usr/bin/env bash
# Build only; --host explicitly uses a host Vulkan loader for client tests.
set -euo pipefail
AME="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${DANGO_PANVK_RENDERER_BUILD:-${AME}/out/panvk-renderer}"
mkdir -p "${OUT}"
for stage in vert frag; do
    glslangValidator -V --target-env vulkan1.0 \
        "${AME}/linux-artifacts/panvk-renderer/triangle.${stage}" -o "${OUT}/triangle.${stage}.spv"
    spirv-val --target-env vulkan1.0 "${OUT}/triangle.${stage}.spv"
done
python3 - "${OUT}" <<'PY'
from pathlib import Path
import struct, sys
out = Path(sys.argv[1])
with (out / 'shaders.h').open('w') as f:
    f.write('/* Generated from triangle.vert/frag; do not edit. */\n#include <stdint.h>\n')
    for stage in ('vert', 'frag'):
        data = (out / f'triangle.{stage}.spv').read_bytes()
        words = struct.unpack(f'<{len(data)//4}I', data)
        f.write(f'static const uint32_t triangle_{stage}[] = {{\n')
        for i in range(0, len(words), 8):
            f.write('    ' + ','.join(f'0x{w:08x}' for w in words[i:i+8]) + ',\n')
        f.write('};\n')
PY
VULKAN_SOURCE="${DANGO_PANVK_BUILD:-${AME}/out/mesa-panvk-midgard}/mesa-24.3.4"
COMPILER=aarch64-linux-gnu-gcc
if [[ "${1:-}" == --host ]]; then COMPILER=cc; fi
"${COMPILER}" -std=c11 -O2 -g -Wall -Wextra -Werror \
    -I"${AME}/shared/include" -I"${OUT}" \
    -I"${VULKAN_SOURCE}/include" \
    "${AME}/linux-artifacts/panvk-renderer/renderer.c" \
    "${AME}/linux-artifacts/panvk-renderer/main.c" \
    -ldl -lm -o "${OUT}/panvk-triangle"
echo "Vulkan triangle: ${OUT}/panvk-triangle"
