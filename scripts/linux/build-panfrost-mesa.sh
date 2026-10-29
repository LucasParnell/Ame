#!/usr/bin/env bash
# Minimal cross-built Mesa: T830 GLES, no LLVM, Vulkan or software drivers.
set -euo pipefail
AME="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${DANGO_MESA_BUILD:-${AME}/out/mesa-panfrost}"
mkdir -p "${OUT}"
OUT="$(realpath "${OUT}")"
PREFIX="${DANGO_MESA_PREFIX:-${OUT}/install}"
for tool in curl tar meson ninja aarch64-linux-gnu-gcc aarch64-linux-gnu-g++ pkg-config; do
    command -v "${tool}" >/dev/null || { echo "missing tool: ${tool}" >&2; exit 1; }
done
fetch() {
    local name="$1" url="$2" hash="$3"
    [[ -f "${OUT}/${name}.tar.xz" ]] || curl --fail --location --retry 2 "${url}" -o "${OUT}/${name}.tar.xz"
    echo "${hash}  ${OUT}/${name}.tar.xz" | sha256sum --check --status
    [[ -d "${OUT}/${name}" ]] || tar -xf "${OUT}/${name}.tar.xz" -C "${OUT}"
}
fetch mesa-24.3.4 https://archive.mesa3d.org/mesa-24.3.4.tar.xz \
    e641ae27191d387599219694560d221b7feaa91c900bcec46bf444218ed66025
fetch libdrm-2.4.124 https://dri.freedesktop.org/libdrm/libdrm-2.4.124.tar.xz \
    ac36293f61ca4aafaf4b16a2a7afff312aa4f5c37c9fbd797de9e3c0863ca379
# Small pinned compression dependency for Mesa's compiled-shader/blob cache.
fetch zlib-1.3.1 https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz \
    9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
if [[ ! -f "${PREFIX}/lib/pkgconfig/zlib.pc" ]]; then
    (cd "${OUT}/zlib-1.3.1"
     CHOST=aarch64-linux-gnu CC=aarch64-linux-gnu-gcc AR=aarch64-linux-gnu-ar \
        ./configure --prefix="${PREFIX}"
     make -j"${DANGO_MESA_JOBS:-8}"
     make install)
fi
python3 - "${OUT}" "${PREFIX}" <<'PY'
from pathlib import Path
import sys
out, prefix = map(Path, sys.argv[1:])
# glibc 2.43 declares C23 once_flag in stdlib.h even under _GNU_SOURCE.
# Keep Mesa 24's pthread-backed shim in its own namespace, without changing
# pthread mutex/condition layouts or Mesa's shader/compiler implementation.
p = out / 'mesa-24.3.4/src/c11/threads.h'
text = p.read_text()
if '#define once_flag mesa_once_flag' not in text:
    text = text.replace('#include <stdlib.h>', '#include <stdlib.h>\n'
        '#define once_flag mesa_once_flag\n#define call_once mesa_call_once\n#undef ONCE_FLAG_INIT')
    p.write_text(text)
# GCC 16 can initialize just the first boolean in the shader-key union for
# `{0}`. Midgard then indexes its format table with uninitialized fs formats.
# Clear the complete object representation used for variant keys and hashing.
p = out / 'mesa-24.3.4/src/gallium/drivers/panfrost/pan_shader.c'
text = p.read_text()
old = 'struct panfrost_shader_key key = {0};'
new = 'struct panfrost_shader_key key;\n   memset(&key, 0, sizeof(key));'
if old in text:
    if text.count(old) != 2:
        raise RuntimeError('Unexpected Panfrost shader-key initializers')
    p.write_text(text.replace(old, new))
elif text.count(new) != 2:
    raise RuntimeError('Missing Panfrost shader-key initialization fix')
(out / 'cross.ini').write_text(f"""[binaries]
c = 'aarch64-linux-gnu-gcc'
cpp = 'aarch64-linux-gnu-g++'
ar = 'aarch64-linux-gnu-ar'
strip = 'aarch64-linux-gnu-strip'
pkg-config = 'pkg-config'
[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'cortex-a53'
endian = 'little'
[properties]
needs_exe_wrapper = true
pkg_config_libdir = ['{prefix}/lib/pkgconfig']
[built-in options]
c_args = ['-mcpu=cortex-a53']
cpp_args = ['-mcpu=cortex-a53']
pkg_config_path = ['{prefix}/lib/pkgconfig']
""")
PY
if [[ ! -f "${OUT}/drm-build/build.ninja" ]]; then
    meson setup "${OUT}/drm-build" "${OUT}/libdrm-2.4.124" --cross-file "${OUT}/cross.ini" \
        --prefix "${PREFIX}" --libdir lib --buildtype release \
        -Dintel=disabled -Dradeon=disabled -Damdgpu=disabled -Dnouveau=disabled \
        -Dvmwgfx=disabled -Domap=disabled -Dexynos=disabled -Dfreedreno=disabled \
        -Dtegra=disabled -Dvc4=disabled -Detnaviv=disabled -Dtests=false -Dman-pages=disabled
fi
ninja -C "${OUT}/drm-build" -j"${DANGO_MESA_JOBS:-$(nproc)}" install
if [[ ! -f "${OUT}/mesa-build/build.ninja" ]]; then
    PKG_CONFIG_LIBDIR="${PREFIX}/lib/pkgconfig" meson setup \
        "${OUT}/mesa-build" "${OUT}/mesa-24.3.4" --cross-file "${OUT}/cross.ini" \
        --prefix "${PREFIX}" --libdir lib --buildtype release \
        -Dgallium-drivers=panfrost '-Dvulkan-drivers=[]' '-Dplatforms=[]' \
        -Dglx=disabled -Degl=enabled -Dgbm=enabled -Dopengl=false -Db_lto=true \
        -Dgles1=disabled -Dgles2=enabled -Dllvm=disabled -Dshared-glapi=enabled \
        '-Dvideo-codecs=[]' -Dzlib=enabled -Dzstd=disabled -Dexpat=disabled \
        -Dxmlconfig=disabled -Dshader-cache=enabled -Dgallium-vdpau=disabled \
        -Dgallium-va=disabled -Dlibunwind=disabled -Dlmsensors=disabled -Dbuild-tests=false
fi
# Reconfigure existing trees too; otherwise iteration silently retains the old
# desktop-GL build. GLES remains enabled; LTO removes unused cross-unit code.
PKG_CONFIG_LIBDIR="${PREFIX}/lib/pkgconfig" meson configure "${OUT}/mesa-build" -Dopengl=false -Db_lto=true -Dzlib=enabled -Dshader-cache=enabled
ninja -C "${OUT}/mesa-build" -j"${DANGO_MESA_JOBS:-$(nproc)}" install
echo "Mesa/Panfrost installed: ${PREFIX}"
