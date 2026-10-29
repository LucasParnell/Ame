#!/usr/bin/env bash
# Build a render-only ANE-LX1 driver appliance on Linux, with no distro/rootfs.
set -euo pipefail
AME="$(cd "$(dirname "$0")/../.." && pwd)"
: "${DANGO_LINUX_SRC:=$HOME/Code/linux}"
OUT="${DANGO_LINUX_OUT:-${AME}/out/dango-linux-panfrost}"
KOUT="${DANGO_LINUX_KOUT:-${OUT}/kernel-build}"
FRAGMENT="${AME}/linux-artifacts/config/dango_panfrost_arm64.fragment"
ROOTFS="${DANGO_PANFROST_ROOTFS:-initramfs}"
FLOW=panfrost
case "${ROOTFS}" in
    initramfs) ;;
    squashfs)
        [[ "${DANGO_PANFROST_RENDERER:-none}" == gles ]] || {
            echo 'error: squashfs requires the renderer runtime' >&2; exit 1;
        }
        FLOW=panfrost-squashfs ;;
    *) echo 'error: DANGO_PANFROST_ROOTFS must be initramfs or squashfs' >&2; exit 1 ;;
esac
[[ "$(uname -s)" == Linux ]] || { echo 'error: build this profile on Linux' >&2; exit 1; }
[[ -f "${DANGO_LINUX_SRC}/Makefile" ]] || { echo 'error: set DANGO_LINUX_SRC to Linux v6.12' >&2; exit 1; }
for tool in make clang ld.lld llvm-ar llvm-nm llvm-objcopy bc flex bison dtc cpio xz; do
    command -v "${tool}" >/dev/null || { echo "error: missing host build tool: ${tool}" >&2; exit 1; }
done
mkdir -p "${OUT}" "${KOUT}"
OUT="$(realpath "${OUT}")"
KOUT="$(realpath "${KOUT}")"
SOURCE="$(realpath "${DANGO_LINUX_SRC}")"
python3 "${AME}/scripts/linux/stage-gpu-source.py" "${SOURCE}" "${KOUT}/.dango-source"
MAKE_ARGS=(LLVM=1 ARCH=arm64 O="${KOUT}" -C "${KOUT}/.dango-source")
if [[ "${ROOTFS}" == squashfs ]]; then
    python3 - "${FRAGMENT}" "${AME}/linux-artifacts/config/dango_panfrost_squashfs.fragment" \
        "${KOUT}/.dango-panfrost.fragment" <<'PY_CONFIG'
from pathlib import Path
import sys
values = {}
for name in sys.argv[1:3]:
    for line in Path(name).read_text().splitlines():
        if line.startswith('CONFIG_'):
            key, value = line.split('=', 1)
            values[key] = value
Path(sys.argv[3]).write_text(''.join(f'{key}={value}\n' for key, value in values.items()))
PY_CONFIG
    FRAGMENT="${KOUT}/.dango-panfrost.fragment"
fi
# Starting from allnoconfig prevents unrelated defconfig drivers reappearing.
make "${MAKE_ARGS[@]}" KCONFIG_ALLCONFIG="${FRAGMENT}" allnoconfig
python3 "${AME}/scripts/linux/check-minimal-config.py" --config "${KOUT}/.config" --flow "${FLOW}"
make "${MAKE_ARGS[@]}" -j"${DANGO_LINUX_JOBS:-1}" Image
cp "${KOUT}/arch/arm64/boot/Image" "${OUT}/Image"
cp "${KOUT}/.config" "${OUT}/linux.config"
export DANGO_LINUX_OUT="${OUT}" DANGO_LINUX_PROFILE=panfrost DANGO_LINUX_GUEST=minimal
if [[ "${DANGO_PANFROST_RENDERER:-none}" == gles ]]; then
    "${AME}/scripts/linux/build-panfrost-renderer.sh"
else
    "${AME}/scripts/linux/build-agent.sh"
    "${AME}/scripts/linux/build-initramfs.sh"
fi
DANGO_DTS_TEMPLATE="${AME}/linux-artifacts/dts/dango-linux-panfrost.dts.in" \
    "${AME}/scripts/linux/build-dtb.sh"
"${AME}/scripts/linux/build-manifest.sh"
python3 - "${SOURCE}" "${OUT}" <<'PY'
import hashlib, json, os, pathlib, subprocess, sys
source, out = map(pathlib.Path, sys.argv[1:])
git = ['git', '-C', str(source)]
info = dict(profile='panfrost', target='ANE-LX1 Mali-T830',
    source_revision=subprocess.check_output(git + ['rev-parse', 'HEAD'], text=True).strip(),
    source_dirty=bool(subprocess.check_output(git + ['status', '--porcelain', '--untracked-files=no'])),
    gpu_assignment='Hi6250 host mediator with private Mali shadow MMU; validate each image on device',
    hardware_requested=os.environ.get('DANGO_PANFROST_HARDWARE', '0') == '1',
    renderer=os.environ.get('DANGO_PANFROST_RENDERER', 'none'),
    rootfs_storage=os.environ.get('DANGO_PANFROST_ROOTFS', 'initramfs'),
    artifacts={})
names = ['Image', 'initramfs.cpio', 'dango-linux.dtb', 'dango-agent', 'linux.config', 'manifest.toml']
if info['rootfs_storage'] == 'squashfs':
    names.append('rootfs-storage.json')
for name in names:
    data = (out / name).read_bytes()
    info['artifacts'][name] = dict(bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
(out / 'build-info.json').write_text(json.dumps(info, indent=2) + '\n')
PY
# Only runtime artifacts and provenance go in the distributable; no build tree.
ARCHIVE_EXTRA=()
if [[ "${ROOTFS}" == squashfs ]]; then ARCHIVE_EXTRA+=(rootfs-storage.json); fi
tar --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner -cJf \
    "${OUT}/dango-panfrost.tar.xz" -C "${OUT}" \
    Image initramfs.cpio dango-linux.dtb dango-agent manifest.toml linux.config build-info.json "${ARCHIVE_EXTRA[@]}"
echo "appliance archive: ${OUT}/dango-panfrost.tar.xz"
