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
for variant in float float_ms uint uint_ms float_array float_ms_array uint_array uint_ms_array; do
    PROBE_DEFS=()
    [[ "$variant" == *ms* ]] && PROBE_DEFS+=(-DMULTISAMPLED)
    [[ "$variant" == uint* ]] && PROBE_DEFS+=(-DINTEGER_SOURCE)
    [[ "$variant" == *array ]] && PROBE_DEFS+=(-DLAYERED_SOURCE)
    glslangValidator -V --target-env vulkan1.0 "${PROBE_DEFS[@]}" \
        "${AME}/linux-artifacts/panvk-renderer/probe.comp" -o "${OUT}/probe.${variant}.spv"
    spirv-val --target-env vulkan1.0 "${OUT}/probe.${variant}.spv"
done
for variant in vertex vertex_inputs fragment fragment_uint; do
    PROBE_DEFS=()
    PROBE_STAGE=frag
    [[ "$variant" == vertex* ]] && PROBE_STAGE=vert
    [[ "$variant" == vertex_inputs ]] && PROBE_DEFS+=(-DVERTEX_INPUT)
    [[ "$variant" == fragment_uint ]] && PROBE_DEFS+=(-DINTEGER_SOURCE)
    glslangValidator -V --target-env vulkan1.0 "${PROBE_DEFS[@]}" \
        "${AME}/linux-artifacts/panvk-renderer/probe.${PROBE_STAGE}" -o "${OUT}/probe.${variant}.spv"
    spirv-val --target-env vulkan1.0 "${OUT}/probe.${variant}.spv"
done
for variant in storage_float storage_unorm storage_atomic; do
    PROBE_DEFS=()
    [[ "$variant" == storage_unorm ]] && PROBE_DEFS+=(-DNORMALIZED_STORAGE)
    [[ "$variant" == storage_atomic ]] && PROBE_DEFS+=(-DATOMIC_STORAGE)
    glslangValidator -V --target-env vulkan1.0 "${PROBE_DEFS[@]}" \
        "${AME}/linux-artifacts/panvk-renderer/storage.comp" -o "${OUT}/probe.${variant}.spv"
    spirv-val --target-env vulkan1.0 "${OUT}/probe.${variant}.spv"
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
with (out / 'probe-shaders.h').open('w') as f:
    f.write('/* Generated from probe shaders; do not edit. */\n#include <stdint.h>\n')
    for variant in ('float', 'float_ms', 'uint', 'uint_ms', 'float_array', 'float_ms_array', 'uint_array', 'uint_ms_array', 'vertex', 'vertex_inputs', 'fragment', 'fragment_uint', 'storage_float', 'storage_unorm', 'storage_atomic'):
        data = (out / f'probe.{variant}.spv').read_bytes()
        words = struct.unpack(f'<{len(data)//4}I', data)
        f.write(f'static const uint32_t probe_{variant}[] = {{\n')
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
"${COMPILER}" -std=c11 -O2 -g -Wall -Wextra -Werror \
    -DPANVK_SUITE_STANDALONE -I"${VULKAN_SOURCE}/include" -I"${OUT}" \
    "${AME}/linux-artifacts/panvk-renderer/suite.c" \
    -ldl -lm -o "${OUT}/panvk-suite"
echo "Vulkan triangle: ${OUT}/panvk-triangle"
