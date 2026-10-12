#!/usr/bin/env bash
# Build the pinned Switchroot secure monitor for the board's Hekate profile.
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${REPO}/scripts/host.sh"
ame_host_tools
exec python3 "${REPO}/scripts/switch_atf.py" build --jobs "$(ame_jobs)" "$@"
