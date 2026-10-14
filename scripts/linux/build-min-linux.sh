#!/usr/bin/env bash
# Build the single-vCPU hello appliance on a Linux host. All generated
# files belong to a separate output tree; the supplied source stays read-only.
set -euo pipefail

AME="$(cd "$(dirname "$0")/../.." && pwd)"
: "${DANGO_LINUX_SRC:=$HOME/Code/linux}"
OUT="${DANGO_LINUX_OUT:-${AME}/build/dango-linux}"
KOUT="${DANGO_LINUX_KOUT:-${AME}/build/dango-linux-hello-kout}"
FRAGMENT="${AME}/linux-artifacts/config/dango_hello_arm64.fragment"
export DANGO_LINUX_OUT="${OUT}" DANGO_LINUX_PROFILE=hello DANGO_LINUX_GUEST=minimal

if [[ "$(uname -s)" != Linux ]]; then
    echo "error: use scripts/linux/build-hello-linux-docker.sh on macOS" >&2
    exit 1
fi
if [[ ! -f "${DANGO_LINUX_SRC}/Makefile" ]]; then
    echo "error: provide a pinned Linux tree with DANGO_LINUX_SRC" >&2
    exit 1
fi
mkdir -p "${OUT}" "${KOUT}"

# All configuration, generated headers and objects belong to this build tree.
MAKE_ARGS=(LLVM=1 ARCH=arm64 O="${KOUT}" -C "${DANGO_LINUX_SRC}")
stamp="${KOUT}/.dango-hello-configured"
if [[ ! -f "${stamp}" || "${FRAGMENT}" -nt "${stamp}" ]]; then
    rm -f "${stamp}"
    make "${MAKE_ARGS[@]}" defconfig
    "${DANGO_LINUX_SRC}/scripts/kconfig/merge_config.sh" -m -O "${KOUT}" \
        "${KOUT}/.config" "${FRAGMENT}"
    make "${MAKE_ARGS[@]}" olddefconfig
    python3 "${AME}/scripts/linux/check-minimal-config.py" --config "${KOUT}/.config" --flow hello
    touch "${stamp}"
else
    python3 "${AME}/scripts/linux/check-minimal-config.py" --config "${KOUT}/.config" --flow hello
fi
make "${MAKE_ARGS[@]}" -j"${DANGO_LINUX_JOBS:-$(nproc)}" Image
cp "${KOUT}/arch/arm64/boot/Image" "${OUT}/Image"
cp "${KOUT}/vmlinux" "${OUT}/vmlinux"
cp "${KOUT}/System.map" "${OUT}/System.map"

"${AME}/scripts/linux/build-agent.sh"
"${AME}/scripts/linux/build-initramfs.sh"
"${AME}/scripts/linux/build-dtb.sh"
# Use the same manifest writer as the GPU build so size/layout checks cannot
# drift when an agent-only iteration changes the initramfs.
"${AME}/scripts/linux/build-manifest.sh"

# Record the exact source/config behind this hello Image. This cold build-time
# metadata never adds work to the host/guest execution path.
python3 - "${DANGO_LINUX_SRC}" "${KOUT}/.config" "${OUT}/build-info.json" <<'PY_INFO'
import hashlib
import json
from pathlib import Path
import subprocess
import sys
source, config, output = map(Path, sys.argv[1:])
revision = subprocess.check_output(['git', '-c', f'safe.directory={source}',
    '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
info = {'profile': 'hello', 'guest': 'minimal', 'source_revision': revision,
        'config_sha256': hashlib.sha256(config.read_bytes()).hexdigest()}
output.write_text(json.dumps(info, indent=2) + '\n')
PY_INFO
