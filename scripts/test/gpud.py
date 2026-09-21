#!/usr/bin/env python3
"""Phase H/I acceptance: the bootstrap GPU control plane and present path.

Boots Ame with the SMMUv3 enforced and the assigned virtio-gpu-pci function and
lets the manager, as `drv.gpu.linux`, drive the bootstrap Midix GPU protocol
over the Dango channel:

    GPU_HELLO -> GPU_ENUM_OUTPUTS -> GPU_GET_MODES -> GPU_BUFFER_REGISTER
              -> GPU_SET_MODE -> GPU_PRESENT -> GPU_PRESENT_COMPLETE

The manager renders a four-colour XRGB8888 bar frame into the shared staging
window and registers that window as the scanout buffer, so the console capture
below proves the protocol path end to end: the pixels on the QEMU display came
from the buffer the appliance was handed, not from a second drawing path.

Asserted here: the five control markers, the SMMU proof counters (no faults),
and the captured frame. The PPM reader and the four-bar assertion are Phase F's
(kms-display.py), imported rather than copied, so both rows agree on the pattern.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

# Loadable by path (host tests) as well as directly; keep the sibling import.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ef_constants as ef  # noqa: E402
import logfmt  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]

# The qdev id the capture must target. Derived from Phase F's device string
# rather than restated, so the two cannot name different consoles.
GPU_QDEV_ID = 'gpu0'


def _load_kms_display():
    """Import scripts/test/kms-display.py by path for its PPM helpers."""
    path = Path(__file__).with_name('kms-display.py')
    spec = importlib.util.spec_from_file_location('kms_display', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


kms = _load_kms_display()

if f'id={GPU_QDEV_ID},' not in kms.GPU_DEVICE:
    raise RuntimeError(
        f'gpud.py: screendump device {GPU_QDEV_ID!r} is not the id in '
        f'the Phase F GPU device string {kms.GPU_DEVICE!r}')


def evidence(logs):
    counters = ef.parse_proof(logs, 'gpu')
    checks = {
        'smmu_ready': ef.MARKERS['smmu_ready'] in logs,
        'assignment_active': ef.MARKERS['assign_active'] in logs,
        'linux_driver': ef.MARKERS['linux_driver'] in logs,
        'control_hello': ef.MARKERS['gpu_control_hello'] in logs,
        'control_outputs': ef.MARKERS['gpu_control_outputs'] in logs,
        'control_mode': ef.MARKERS['gpu_control_mode'] in logs,
        'staging_registered': ef.MARKERS['gpu_staging_registered'] in logs,
        'present_complete': ef.MARKERS['gpu_present_complete'] in logs,
        'no_failures': not any(word in logs for word in ef.FAILURE_WORDS),
        # The shared staging window is host-written and appliance-read, so the
        # run is only clean when the SMMU saw the traffic without a fault.
        'iommu_faults_zero': bool(counters) and counters['faults'] == 0 and
                             counters['quarantines'] == 0,
    }
    return checks, counters


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', default=str(ROOT / 'out/qemu-gpu'))
    parser.add_argument('--timeout', type=float, default=180)
    parser.add_argument('--cpus', type=int, choices=[1, 2, 4], default=2)
    parser.add_argument('--output')
    args = parser.parse_args()
    work = Path(args.output) if args.output else Path(tempfile.mkdtemp(prefix='ame-gpud-'))
    work.mkdir(parents=True, exist_ok=True)
    monitor = work / 'monitor.sock'
    shot = work / 'gpud.ppm'
    env = dict(os.environ, AME_QEMU_IOMMU='1', AME_QEMU_TESTDEV='0',
               AME_QEMU_GPU_DEVICE=kms.GPU_DEVICE)
    # `gpu_proof` waits for the kernel's SMMU counter line and
    # `gpu_present_complete` for the manager's last marker, so the capture is
    # taken only once the staged frame has been presented and proven faultless.
    command = [sys.executable, str(ROOT / 'scripts/test/boot.py'), '--build', args.build,
               '--cpus', str(args.cpus), '--timeout', str(args.timeout), '--output', str(work),
               '--marker', ef.MARKERS['boot_ready'], '--marker', ef.MARKERS['gpu_proof'],
               '--marker', ef.MARKERS['gpu_present_complete'],
               '--monitor-socket', str(monitor), '--screendump', str(shot),
               '--screendump-device', GPU_QDEV_ID]
    result = subprocess.run(command, env=env, capture_output=True, text=True,
                            timeout=args.timeout + 60)
    try:
        boot = json.loads(result.stdout.strip().splitlines()[-1])
    except (ValueError, IndexError):
        boot = {'success': False, 'reason': result.stderr or result.stdout}
    logs = ''.join((work / name).read_text(errors='replace') for name in ('host.log', 'guest.log')
                   if (work / name).exists())
    logs = logfmt.marker_stream(logs)
    checks, counters = evidence(logs)
    checks['boot_ready'] = result.returncode == 0 and boot.get('success', False)
    checks['screendump'] = boot.get('screendump_ok') is True and shot.exists()
    pattern = {}
    if checks['screendump']:
        try:
            ok, pattern = kms.assert_pattern(shot, expect_size=kms.EXPECT_MODE)
            checks['present_pattern'] = ok
        except (OSError, ValueError) as error:
            checks['present_pattern'] = False
            pattern = {'error': str(error)}
    else:
        checks['present_pattern'] = False
    success = all(checks.values())
    summary = dict(success=success, reason=boot.get('reason'), elapsed=boot.get('elapsed'),
                   logs=str(work), display=str(shot), checks=checks, counters=counters,
                   pattern=pattern)
    (work / 'gpud-result.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary, indent=2))
    return 0 if success else 1


if __name__ == '__main__':
    raise SystemExit(main())
