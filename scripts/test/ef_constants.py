#!/usr/bin/env python3
"""Host-test constants derived from the shared guest-facing ABI headers.

The Phase E/F/G acceptance harnesses must assert against the same values the
guest/userspace builds are compiled with, so those values are read back from
their single source of truth instead of being restated (and drifting) in the
host tests:

  * shared/include/midix/abi/dango_channel_guest.h  — platform windows, IRQ
  * shared/include/midix/abi/dango_channel_test.h   — channel test round count

Only the plain `#define NAME value` object-like macros are parsed; function-like
macros (e.g. `DANGO_RING_SLOTS(order)`) are ignored. A missing macro is a hard
error so a renamed/removed ABI symbol surfaces here rather than as a silently
skipped check. Stdlib only; no build step required.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
_GUEST_HEADER = ROOT / 'shared/include/midix/abi/dango_channel_guest.h'
_TEST_HEADER = ROOT / 'shared/include/midix/abi/dango_channel_test.h'

# Object-like macros only: the name must be followed by whitespace, which skips
# function-like definitions such as `#define DANGO_RING_SLOTS(order) ...`.
_DEFINE_RE = re.compile(r'^#define\s+([A-Za-z_]\w*)\s+(\S+)', re.MULTILINE)


def _defines(path):
    text = Path(path).read_text()
    return {m.group(1): m.group(2) for m in _DEFINE_RE.finditer(text)}


def _int(defines, macro, path):
    try:
        token = defines[macro]
    except KeyError:
        raise KeyError(
            f'ef_constants: {macro} is not defined in {path}; the host-test '
            f'constants are derived from the shared ABI headers') from None
    token = re.sub(r'[uUlL]+$', '', token)
    try:
        return int(token, 0)
    except ValueError:
        raise ValueError(
            f'ef_constants: cannot parse {macro} value {token!r} in {path}') from None


_GUEST = _defines(_GUEST_HEADER)
_TEST = _defines(_TEST_HEADER)

CHANNEL_GUEST_BASE = _int(_GUEST, 'DANGO_CH_GUEST_BASE', _GUEST_HEADER)
CHANNEL_GUEST_SIZE = _int(_GUEST, 'DANGO_CH_GUEST_SIZE', _GUEST_HEADER)
CHANNEL_GUEST_IRQ = _int(_GUEST, 'DANGO_CH_GUEST_IRQ', _GUEST_HEADER)
GPU_ECAM_BASE = _int(_GUEST, 'DANGO_GPU_ECAM_BASE', _GUEST_HEADER)
GPU_ECAM_SIZE = _int(_GUEST, 'DANGO_GPU_ECAM_SIZE', _GUEST_HEADER)
GPU_MMIO_BASE = _int(_GUEST, 'DANGO_GPU_MMIO_BASE', _GUEST_HEADER)
GPU_BAR4_IPA = _int(_GUEST, 'DANGO_GPU_BAR4_IPA', _GUEST_HEADER)
GPU_CHANNEL_TEST_ROUNDS = _int(_TEST, 'DANGO_CH_TEST_ROUNDS', _TEST_HEADER)

# --- shared proof-counter grammar --------------------------------------------
# The kernel prints one "<token> proof: key=value ..." line per appliance run
# (kernel/vm/vcpu.cc). Keep the grammar here so every acceptance row parses it
# with one regex instead of a near-identical copy in each row script.
PROOF_RE = re.compile(
    r'(\w+) proof: faults=(\d+) quarantines=(\d+) routes=(\d+) injected=(\d+) smmu=(\d+)')
_PROOF_KEYS = ('faults', 'quarantines', 'routes', 'injected', 'smmu')


def parse_proof(logs, token):
    """Return the counters dict for '<token> proof: ...', or {} when absent."""
    match = PROOF_RE.search(logs)
    while match:
        if match.group(1) == token:
            return dict(zip(_PROOF_KEYS, map(int, match.groups()[1:])))
        match = PROOF_RE.search(logs, match.end())
    return {}


# --- shared log vocabulary ----------------------------------------------------
# One marker set and one failure list for every row, so the words a row checks
# can never diverge from what the kernel/guest actually prints.
MARKERS = {
    'boot_ready': 'all services ready',
    'smmu_ready': 'smmuv3 bring-up rc=0 ready=1',
    'assign_active': 'assign: ACTIVE',
    'linux_driver': 'dango: DRM card0 driver=virtio_gpu dumb_buffer=1',
    'linux_isr': 'dango: Linux virtio ISR observed',
    'gpu_probe': 'dango: gpu probe passed',
    'gpu_proof': 'gpu proof:',
    'channel_proof': 'channel proof:',
    'native_channel': 'channel native probe passed',
    'kms_displayed': 'dango: kms displayed',
    'kms_passed': 'dango: kms test passed',
    'channel_test_passed': 'dango: channel test passed',
}

FAILURE_WORDS = (
    'KERNEL PANIC', 'TEST FAILED', 'gpu probe FAILED', 'kms test FAILED',
    'channel test FAILED', 'channel guest probe FAILED', 'channel native probe FAILED',
    'iommu/faults:', 'unsupported trap', 'init: service start failed',
)
