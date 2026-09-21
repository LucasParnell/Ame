#!/usr/bin/env python3
"""Host-test constants derived from the shared guest-facing ABI headers.

The Phase E/F/G/H/I acceptance harnesses must assert against the same values the
guest/userspace builds are compiled with, so those values are read back from
their single source of truth instead of being restated (and drifting) in the
host tests:

  * shared/include/midix/abi/dango_channel_guest.h  — platform windows, IRQ
  * shared/include/midix/abi/dango_channel_test.h   — channel test round count
  * userland/lib/include/dango/layout.h             — manager host-side VAs

Only the plain `#define NAME value` object-like macros are parsed; function-like
macros (e.g. `DANGO_RING_SLOTS(order)`) are ignored. A missing macro is a hard
error so a renamed/removed ABI symbol surfaces here rather than as a silently
skipped check. The manager layout header is C++, so its `constexpr uint64_t`
constants are parsed too, with the same rule: only an integer literal is
accepted, never a symbol that would be a second home for the value. Stdlib only;
no build step required.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
_GUEST_HEADER = ROOT / 'shared/include/midix/abi/dango_channel_guest.h'
_TEST_HEADER = ROOT / 'shared/include/midix/abi/dango_channel_test.h'
# The manager's own host-side window constants live in a C++ header rather than
# a macro header, so they are parsed with the `constexpr` grammar below.
_LAYOUT_HEADER = ROOT / 'userland/lib/include/dango/layout.h'

# Object-like macros only: the name must be followed by whitespace, which skips
# function-like definitions such as `#define DANGO_RING_SLOTS(order) ...`.
_DEFINE_RE = re.compile(r'^#define\s+([A-Za-z_]\w*)\s+(\S+)', re.MULTILINE)
# `constexpr uint64_t kName = value;` in the manager layout header. Group 2 is
# the initialiser expression; only an integer literal is accepted (a symbol
# would be a second home for the value and fails loudly below).
_CONSTEXPR_RE = re.compile(
    r'^constexpr\s+uint64_t\s+(k[A-Za-z0-9_]+)\s*=\s*([^;]+);', re.MULTILINE)


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


def _constexpr_int(path, name):
    """Read a `constexpr uint64_t <name> = <literal>;` constant from `path`."""
    for match in _CONSTEXPR_RE.finditer(Path(path).read_text()):
        if match.group(1) != name:
            continue
        token = re.sub(r"['\s]", '', match.group(2))  # drop digit separators
        token = re.sub(r'[uUlL]+$', '', token)
        try:
            return int(token, 0)
        except ValueError:
            raise ValueError(
                f'ef_constants: {name} in {path} is not an integer literal '
                f'(got {match.group(2).strip()!r})') from None
    raise KeyError(
        f'ef_constants: {name} is not defined in {path}; the host-test constants '
        f'are derived from the manager layout header')


_GUEST = _defines(_GUEST_HEADER)
_TEST = _defines(_TEST_HEADER)

CHANNEL_GUEST_BASE = _int(_GUEST, 'DANGO_CH_GUEST_BASE', _GUEST_HEADER)
CHANNEL_GUEST_SIZE = _int(_GUEST, 'DANGO_CH_GUEST_SIZE', _GUEST_HEADER)
CHANNEL_GUEST_IRQ = _int(_GUEST, 'DANGO_CH_GUEST_IRQ', _GUEST_HEADER)
GPU_ECAM_BASE = _int(_GUEST, 'DANGO_GPU_ECAM_BASE', _GUEST_HEADER)
GPU_ECAM_SIZE = _int(_GUEST, 'DANGO_GPU_ECAM_SIZE', _GUEST_HEADER)
GPU_MMIO_BASE = _int(_GUEST, 'DANGO_GPU_MMIO_BASE', _GUEST_HEADER)
GPU_BAR4_IPA = _int(_GUEST, 'DANGO_GPU_BAR4_IPA', _GUEST_HEADER)
GPU_STAGING_BASE = _int(_GUEST, 'DANGO_GPU_STAGING_BASE', _GUEST_HEADER)
GPU_MODE_WIDTH = _int(_GUEST, 'DANGO_GPU_MODE_WIDTH', _GUEST_HEADER)
GPU_MODE_HEIGHT = _int(_GUEST, 'DANGO_GPU_MODE_HEIGHT', _GUEST_HEADER)
GPU_STAGING_SIZE = _int(_GUEST, 'DANGO_GPU_STAGING_SIZE', _GUEST_HEADER)
GPU_CHANNEL_TEST_ROUNDS = _int(_TEST, 'DANGO_CH_TEST_ROUNDS', _TEST_HEADER)
# Host-side VA the manager reserves for the staging window (dango/layout.h).
GPU_STAGING_HOST_VA = _constexpr_int(_LAYOUT_HEADER, 'kStagingHostVa')

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


def parse_counter(logs, key):
    """Return the last `<key>=<int>` counter value in the log, or None.

    Counters are cumulative and shared with earlier channel users (the bootstrap
    GPU exchange), so callers compare against a floor, never an absolute value.
    """
    value = None
    for match in re.finditer(rf'(?<![\w.]){re.escape(key)}=(\d+)', logs):
        value = int(match.group(1))
    return value


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
    # Phase H/I: the manager's bootstrap GPU control plane and present path. The
    # strings are the markers the manager's gpu_bootstrap_client() prints, one
    # per stage, with the version/mode pinned to the ABI by manager-side
    # static_asserts (see userland/dango/manager/main.cc).
    'gpu_control_hello': 'gpu control: hello major=1 minor=0',
    'gpu_control_outputs': 'gpu control: outputs=1',
    'gpu_control_mode': 'gpu control: mode 1024x768',
    'gpu_staging_registered': 'gpu staging: registered',
    'gpu_present_complete': 'gpu present: complete',
}

FAILURE_WORDS = (
    'KERNEL PANIC', 'TEST FAILED', 'gpu probe FAILED', 'kms test FAILED',
    'channel test FAILED', 'channel guest probe FAILED', 'channel native probe FAILED',
    'iommu/faults:', 'unsupported trap', 'init: service start failed',
    # The manager's bootstrap GPU client aborts every stage with this prefix
    # (userland/dango/manager/main.cc), so a stalled control-plane exchange is
    # attributed to the exchange and not only reported as a boot timeout.
    'gpu client FAILED',
)

# --- shared GPU appliance device -------------------------------------------------
# One device string for every row that boots the appliance: the headless QEMU
# console needs an EDID to offer modes, and `id=` targets the capture away from
# ramfb. The one mode the rows assert is stated here once.
GPU_MODE = (GPU_MODE_WIDTH, GPU_MODE_HEIGHT)
GPU_QDEV_ID = 'gpu0'
GPU_DEVICE = (f'virtio-gpu-pci,iommu_platform=on,id={GPU_QDEV_ID},edid=on,'
              f'xres={GPU_MODE[0]},yres={GPU_MODE[1]}')
