#!/usr/bin/env bash
# Isolated, pinned experimental Midgard ICD. Does not replace installed GLES.
set -euo pipefail
AME="$(cd "$(dirname "$0")/../.." && pwd)"
BASE="${DANGO_MESA_BUILD:-${AME}/out/mesa-panfrost}"
OUT="${DANGO_PANVK_BUILD:-${AME}/out/mesa-panvk-midgard}"
mkdir -p "${OUT}"
OUT="$(realpath "${OUT}")"
PREFIX="${DANGO_PANVK_PREFIX:-${OUT}/install}"
PATCHES=("${AME}/third_party/mesa/panvk-midgard/0001-panvk-midgard-bringup.patch"
         "${AME}/third_party/mesa/panvk-midgard/0002-panvk-v5-tagged-framebuffer-pointer.patch")
if [[ ! -f "${BASE}/install/lib/pkgconfig/libdrm.pc" || ! -f "${BASE}/cross.ini" ]]; then
    "${AME}/scripts/linux/build-panfrost-mesa.sh"
fi
ARCHIVE="${BASE}/mesa-24.3.4.tar.xz"
[[ -f "${ARCHIVE}" ]] || curl --fail --location --retry 2 \
    https://archive.mesa3d.org/mesa-24.3.4.tar.xz -o "${ARCHIVE}"
echo "e641ae27191d387599219694560d221b7feaa91c900bcec46bf444218ed66025  ${ARCHIVE}" | sha256sum --check --status
SOURCE="${OUT}/mesa-24.3.4"
[[ -d "${SOURCE}" ]] || tar -xf "${ARCHIVE}" -C "${OUT}"
# A changed patch must be rebased explicitly onto a clean tree. Re-running an
# identical build accepts a source tree only when the whole series reverses.
apply_series() {
    local patch
    for patch in "${PATCHES[@]}"; do
        patch --batch --forward --silent -p1 -d "${SOURCE}" < "${patch}"
    done
}
# A series cannot be dry-run reversed in place: 0002 edits lines that 0001
# introduced, so 0001 only reverses once 0002 is really gone. The checker unwinds
# a scratch copy of the touched files instead.
reverse_ok() {
    python3 "${AME}/scripts/linux/check-panvk-patch-state.py" "${SOURCE}" "${PATCHES[@]}"
}
# Only the first patch is checked against the current tree. The rest are
# checked in sequence, because 0002 needs 0001 already applied to have context.
forward_ok() {
    patch --dry-run --silent --batch --forward -p1 -d "${SOURCE}" < "${PATCHES[0]}" \
        >/dev/null 2>&1
}
if forward_ok; then
    apply_series
elif ! reverse_ok; then
    echo "Source does not match the pinned patch series; use a fresh DANGO_PANVK_BUILD directory" >&2
    exit 1
fi
python3 - "${SOURCE}" "${OUT}" "${PATCHES[@]}" <<'PY'
from pathlib import Path
import hashlib, json, sys
source, out = Path(sys.argv[1]), Path(sys.argv[2])
patches = [Path(a) for a in sys.argv[3:]]
# Compatibility with the build host's glibc C23 once_flag declarations.
p = source / 'src/c11/threads.h'
s = p.read_text()
if '#define once_flag mesa_once_flag' not in s:
    needle = '#include <stdlib.h>'
    if s.count(needle) != 1:
        raise RuntimeError('C11 thread compatibility patch no longer matches')
    p.write_text(s.replace(needle, needle + '\n#define once_flag mesa_once_flag\n'
        '#define call_once mesa_call_once\n#undef ONCE_FLAG_INIT'))
files = sorted((source / 'src/panfrost/vulkan').rglob('*'))
(out / 'source-manifest.json').write_text(json.dumps({
    'mesa_version': '24.3.4',
    'mesa_archive_sha256': 'e641ae27191d387599219694560d221b7feaa91c900bcec46bf444218ed66025',
    'patch_sha256': [hashlib.sha256(p.read_bytes()).hexdigest() for p in patches],
    'patches': [p.name for p in patches],
    'status': 'experimental; hardware rendering not established by a build',
    'sources': {str(p.relative_to(source)): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in files if p.is_file()},
}, indent=2) + '\n')
PY
# A build dir that already has build.ninja is reused as-is, so a dir configured
# with a different feature set (egl/gbm/gallium enabled) silently produces a
# different library than this script describes. Warn instead of rebuilding blind:
# the resulting ICD is functionally equivalent for panvk, but the difference
# should never be a surprise again.
if [[ -f "${OUT}/build/build.ninja" ]]; then
    python3 "${AME}/scripts/linux/check-panvk-build-config.py" "${OUT}/build" || true
fi
if [[ ! -f "${OUT}/build/build.ninja" ]]; then
    meson setup "${OUT}/build" "${SOURCE}" --cross-file "${BASE}/cross.ini" \
        --prefix "${PREFIX}" --libdir lib --buildtype debugoptimized \
        '-Dgallium-drivers=[]' -Dvulkan-drivers=panfrost '-Dplatforms=[]' \
        -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false \
        -Dgles1=disabled -Dgles2=disabled -Dllvm=disabled '-Dvideo-codecs=[]' \
        -Dzlib=enabled -Dzstd=disabled -Dexpat=disabled -Dxmlconfig=disabled \
        -Dshader-cache=enabled -Dlibunwind=disabled -Dlmsensors=disabled \
        -Dbuild-tests=false -Dtools=drm-shim
fi
ninja -C "${OUT}/build" -j"${DANGO_MESA_JOBS:-$(nproc)}" \
    src/panfrost/vulkan/libvulkan_panfrost.so \
    src/panfrost/drm-shim/libpanfrost_noop_drm_shim.so
install -Dm755 "${OUT}/build/src/panfrost/vulkan/libvulkan_panfrost.so" \
    "${PREFIX}/lib/libvulkan_panfrost.so"
mkdir -p "${PREFIX}/share/vulkan/icd.d"
cat > "${PREFIX}/share/vulkan/icd.d/panfrost_midgard.json" <<'JSON'
{"file_format_version":"1.0.0","ICD":{"library_path":"libvulkan_panfrost.so","api_version":"1.0.0"}}
JSON
echo "Experimental PanVK ICD: ${PREFIX}"
