#!/usr/bin/env bash
set -euo pipefail

AME="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${DANGO_LINUX_OUT:-${AME}/build/dango-linux}"
INITRAMFS="${OUT}/initramfs.cpio"

if [[ ! -x "${OUT}/dango-agent" ]]; then
    echo "error: ${OUT}/dango-agent missing; run scripts/linux/build-agent.sh first" >&2
    exit 1
fi

rm -f "${INITRAMFS}"
python3 "${AME}/scripts/linux/mkinitramfs.py" "${OUT}/dango-agent" "${INITRAMFS}"
python3 "${AME}/scripts/linux/check-initramfs.py" "${INITRAMFS}"

echo "initramfs entries:"
(cd "${OUT}" && cpio -it --quiet < initramfs.cpio | sort)
