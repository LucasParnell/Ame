"""One Huawei workflow: build, run from BootROM, or build and run."""
from __future__ import annotations

import argparse
import datetime
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TOOLS = Path(__file__).resolve().parent / 'tools'
WORK = ROOT / 'out/huawei'
LATEST = WORK / 'latest.json'
FIRMWARE = ROOT / 'out/anne-device-private/el2-firmware'
TARGETS = ('anko', 'mochi_system', 'provenance', 'ipc_bench_app', 'ipc_loan_app',
           'ipc_components', 'stream_test')


def tool(name):
    sys.path.insert(0, str(TOOLS))
    spec = importlib.util.spec_from_file_location('huawei_' + name.replace('-', '_'),
                                                  TOOLS / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def execute(command, *, env=None, dry_run=False):
    print('+ ' + shlex.join(map(str, command)), flush=True)
    if not dry_run:
        subprocess.run(list(map(str, command)), cwd=ROOT, env=env, check=True)


def path(value):
    return Path(value).expanduser().resolve()


def positive(value):
    number = int(value)
    if number < 1:
        raise argparse.ArgumentTypeError('must be at least 1')
    return number


def nonnegative(value):
    number = float(value)
    if number < 0:
        raise argparse.ArgumentTypeError('must be zero or greater')
    return number


def package_info(package):
    """Validate the exact image, never select packages by directory timestamp."""
    manifest = json.loads((package / 'manifest.json').read_text())
    image = (package / 'ame-native-kernel.img').read_bytes()
    digest = hashlib.sha256(image).hexdigest()
    if (manifest.get('board') != 'huawei-anne' or manifest.get('model') != 'ANE-LX1' or
            manifest.get('partition') != 'kernel' or len(image) != 0x1800000 or
            manifest.get('partition_bytes') != len(image) or image[:8] != b'ANDROID!' or
            manifest.get('native_image_sha256') != digest or
            not manifest.get('avb', {}).get('local_signature_and_hash_verified')):
        raise ValueError('Huawei package identity, signature record, size or image hash mismatch')
    return manifest


def remember(package, build_dir, profile):
    manifest = package_info(package)
    record = dict(schema=1, package=str(package), build=str(build_dir), profile=profile,
                  native_image_sha256=manifest['native_image_sha256'])
    LATEST.parent.mkdir(parents=True, exist_ok=True)
    # A failed build/package never replaces the last successfully verified image.
    with tempfile.NamedTemporaryFile(mode='w', dir=LATEST.parent, delete=False) as file:
        temporary = Path(file.name)
        json.dump(record, file, indent=2)
        file.write('\n')
    try:
        os.replace(temporary, LATEST)
    finally:
        temporary.unlink(missing_ok=True)


def selected_package(explicit):
    if explicit:
        package_info(explicit)
        return explicit
    if not LATEST.is_file():
        raise ValueError('No recorded Huawei build. Run scripts/huawei build first, '
                         'or use run --package DIR.')
    record = json.loads(LATEST.read_text())
    if record.get('schema') != 1:
        raise ValueError('Unsupported Huawei build record')
    package = path(record['package'])
    manifest = package_info(package)
    if manifest['native_image_sha256'] != record.get('native_image_sha256'):
        raise ValueError('Recorded Huawei package has changed; rebuild or select --package explicitly')
    return package


def guest_environment(directory, source, jobs):
    return dict(os.environ, DANGO_LINUX_OUT=str(directory), DANGO_LINUX_SRC=str(source),
                DANGO_LINUX_KOUT=str(directory / 'kernel-build'),
                DANGO_MESA_BUILD=str(WORK / 'mesa-panfrost'),
                DANGO_MESA_PREFIX=str(WORK / 'mesa-panfrost/install'),
                DANGO_PANVK_BUILD=str(WORK / 'mesa-panvk'),
                DANGO_PANVK_PREFIX=str(WORK / 'mesa-panvk/install'),
                DANGO_LINUX_PROFILE='panfrost', DANGO_LINUX_GUEST='minimal',
                DANGO_PANFROST_RENDERER='gles', DANGO_PANFROST_HARDWARE='1',
                DANGO_LINUX_TIMER_HZ='1920000', DANGO_MANIFEST_NAME='huawei-gpu',
                DANGO_LINUX_JOBS=str(jobs), DANGO_MESA_JOBS=str(jobs))


def guest_worker(source, directory, jobs, dry_run=False):
    if not source.exists():
        if not dry_run:
            source.parent.mkdir(parents=True, exist_ok=True)
        execute(['git', 'clone', '--depth', '1', '--branch', 'v6.12',
                 'https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git', source],
                dry_run=dry_run)
    env = guest_environment(directory, source, jobs)
    for script in ('build-panfrost-mesa.sh', 'build-panvk-mesa.sh', 'build-panfrost-linux.sh'):
        execute([ROOT / 'scripts/linux' / script], env=env, dry_run=dry_run)


def require_guest_space():
    # Docker's sparse VM disk also consumes space on the host as it grows.
    available = shutil.disk_usage(ROOT).free / (1024 ** 3)
    if available < 12:
        raise ValueError(f'Full GPU builds need at least 12 GiB of free disk space '
                         f'({available:.1f} GiB available). Free space or use '
                         '--guest-artifacts DIR / --native-only.')


def export_guest(directory, destination):
    """Copy runtime artifacts out of the Linux volume, publishing the manifest last."""
    required = ('Image', 'initramfs.cpio', 'dango-linux.dtb', 'dango-agent', 'manifest.toml')
    for name in required:
        if not (directory / name).is_file():
            raise ValueError('Guest build is missing ' + name)
    destination.mkdir(parents=True, exist_ok=True)
    names = (*required[:-1], 'build-info.json', 'linux.config', 'mesa-runtime.json',
             'dango-panfrost.tar.xz', 'rootfs-storage.json', 'manifest.toml')
    for name in names:
        source = directory / name
        if source.is_file():
            temporary = destination / ('.' + name + '.tmp')
            shutil.copyfile(source, temporary)
            os.replace(temporary, destination / name)
        else:
            (destination / name).unlink(missing_ok=True)


def build_guest(args):
    directory = args.guest_artifacts or WORK / 'guest'
    if args.guest_artifacts:
        if not args.dry_run:
            import tomllib
            manifest = tomllib.loads((directory / 'manifest.toml').read_text())
            if (manifest.get('profile') != 'panfrost' or manifest.get('guest') != 'minimal' or
                    manifest.get('renderer') != 'gles' or manifest.get('mali_hardware') != '1'):
                raise ValueError('Prebuilt guest must be the minimal hardware Panfrost/GLES/PanVK appliance')
        print('Using explicitly supplied guest artifacts: ' + str(directory), flush=True)
    elif platform.system() == 'Linux':
        if not args.dry_run:
            require_guest_space()
        guest_worker(args.linux_source, directory, args.jobs, args.dry_run)
    else:
        # Mesa/kernel builds need Linux filesystem semantics. Keep source and
        # object caches in a volume rather than extracting onto a macOS bind.
        if not args.dry_run:
            require_guest_space()
            if not shutil.which('docker'):
                raise ValueError('GPU guest builds need Linux or Docker; install/start Docker, '
                                 'or supply --guest-artifacts DIR. --native-only builds without the GPU guest.')
            try:
                subprocess.run(['docker', 'info'], check=True, stdout=subprocess.DEVNULL,
                               stderr=subprocess.PIPE)
            except subprocess.CalledProcessError as error:
                raise RuntimeError('Docker is not running. Start Docker (or colima start), '
                                   'then retry; alternatively supply --guest-artifacts DIR.') from error
            WORK.mkdir(parents=True, exist_ok=True)
            directory.mkdir(parents=True, exist_ok=True)
        execute(['docker', 'build', '--platform', 'linux/arm64', '-t', 'ame-huawei-builder',
                 '-f', TOOLS / 'Dockerfile', TOOLS], dry_run=args.dry_run)
        volume = 'ame-huawei-' + hashlib.sha256(str(ROOT).encode()).hexdigest()[:12]
        execute(['docker', 'run', '--rm', '--mount', f'type=volume,source={volume},target=/cache',
                 'ame-huawei-builder', 'chown', f'{os.getuid()}:{os.getgid()}', '/cache'],
                dry_run=args.dry_run)
        source = args.linux_source
        mounts = ['--mount', f'type=bind,source={ROOT},target={ROOT}',
                  '--mount', f'type=volume,source={volume},target={WORK}',
                  '--mount', f'type=bind,source={directory},target=/huawei-export']
        if source.exists() and not source.is_relative_to(ROOT):
            mounts += ['--mount', f'type=bind,source={source},target={source},readonly']
        execute(['docker', 'run', '--rm', '--platform', 'linux/arm64', '--cpus', str(args.jobs),
                 '--memory', '6g', '--user', f'{os.getuid()}:{os.getgid()}',
                 '-e', 'XDG_CACHE_HOME=/tmp/ame-huawei-cache', *mounts, '-w', ROOT, 'ame-huawei-builder',
                 'python3', ROOT / 'scripts/anne/cli.py', '_guest',
                 '--linux-source', source, '--output', WORK / 'guest',
                 '--export', '/huawei-export', '--jobs', str(args.jobs)],
                dry_run=args.dry_run)
    execute(['bash', ROOT / 'scripts/linux/check-artifacts.sh', directory], dry_run=args.dry_run)
    return directory


def build(args):
    if not args.dry_run:
        if args.output and args.output.exists():
            raise ValueError('Package output already exists; choose a fresh --output directory')
        required = [args.avb_key, args.backups / 'kernel.img', args.backups / 'manifest.json']
        if not args.dev_signing:
            required += [args.signing_key, args.trusted_key]
        missing = [str(p) for p in required if not p.is_file()]
        if missing:
            raise ValueError('Missing Huawei build inputs:\n  ' + '\n  '.join(missing))
    profile = 'huawei-anne-usb' if args.native_only else 'huawei-anne-gpu'
    directory = None if args.native_only else build_guest(args)
    configure = ['cmake', '-S', ROOT, '-B', args.build_dir, '-G', 'Ninja',
                 '-DCMAKE_TOOLCHAIN_FILE=' + str(ROOT / 'scripts/build/toolchain-aarch64.cmake'),
                 '-DPython3_EXECUTABLE=' + sys.executable, '-DCMAKE_BUILD_TYPE=Release',
                 '-DANKO_BOARD=huawei-anne', '-DMOCHI_SYSTEM_PROFILE=' + profile,
                 '-DANKO_BUILD_DEV=' + ('ON' if args.dev_signing else 'OFF'),
                 '-DANKO_ANNE_DIAG_CAPTURE_SECONDS=0', '-DANKO_ANNE_DIAG_RESET_AT_READY=OFF',
                 '-DDANGO_BUILD_LINUX_ARTIFACTS=OFF', '-DDANGO_LINUX_PROFILE=panfrost',
                 '-DDANGO_LINUX_GUEST=minimal', '-DANKO_GUEST_LAYOUT=minimal',
                 '-DDANGO_LINUX_ARTIFACT_DIR=' + (str(directory) if directory else '')]
    if not args.dev_signing:
        configure += ['-DAME_SIGNING_KEY=' + str(args.signing_key),
                      '-DAME_TRUSTED_KEY_FILES=' + str(args.trusted_key)]
    execute(configure, dry_run=args.dry_run)
    execute(['cmake', '--build', args.build_dir, '--parallel', str(args.jobs),
             '--target', *TARGETS], dry_run=args.dry_run)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    package = args.output or WORK / 'packages' / stamp
    execute([sys.executable, TOOLS / 'build-native-package.py', '--build', args.build_dir,
             '--backups', args.backups, '--output', package, '--avb-key', args.avb_key,
             '--avbtool', args.avbtool, '--download-avbtool', '--quiet'], dry_run=args.dry_run)
    if not args.dry_run:
        remember(package, args.build_dir, profile)
        print('Huawei package ready: ' + str(package), flush=True)
    return package


def run(args, package=None):
    if args.dry_run and package is not None and not package.exists():
        print('Would verify package/firmware, wait for BootROM, verify RAM LK, '
              'flash kernel, verify the full readback and issue oem boot.', flush=True)
        return
    package = selected_package(package or args.package)
    flash = tool('flash-el2')
    image, manifest, blobs = flash.inputs(package, args.firmware)
    if args.dry_run:
        print('Verified package and firmware; no USB access: ' + str(package), flush=True)
        return
    try:
        import serial  # noqa: F401
        import usb.core  # noqa: F401
    except ImportError as error:
        raise ValueError(f'Missing {error.name}; install requirements-huawei.txt into .venv') from error
    gate = tool('panvk-gate')
    serial_number = gate.wait_for_loader(flash, blobs, manifest, args.wait)
    if args.serial and args.serial != serial_number:
        raise ValueError('Connected handset serial does not match --serial')
    # Delegate the write and boot to the existing verified transport. This
    # preserves its journal, partition geometry checks and mandatory readback.
    command = [sys.executable, TOOLS / 'flash-el2.py', '--package', package,
               '--firmware', args.firmware, '--running', '--serial', serial_number,
               '--expect-sha256', hashlib.sha256(image).hexdigest()]
    if args.backup:
        command.append('--backup')
    execute(command)


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    commands = result.add_subparsers(dest='command', required=True)

    def build_options(command):
        command.add_argument('--build-dir', type=path, default=WORK / 'build')
        command.add_argument('-j', '--jobs', type=positive, default=min(os.cpu_count() or 1, 4))
        guest = command.add_mutually_exclusive_group()
        guest.add_argument('--native-only', action='store_true', help='USB/native system without the Linux GPU guest')
        guest.add_argument('--guest-artifacts', type=path, help='use an existing matching guest instead of rebuilding it')
        existing_linux = Path.home() / 'Code/linux'
        linux_source = existing_linux if (existing_linux / 'Makefile').is_file() else WORK / 'linux'
        command.add_argument('--linux-source', type=path,
                             default=path(os.environ.get('DANGO_LINUX_SRC', linux_source)))
        command.add_argument('--signing-key', type=path, default=Path.home() / '.ame-keys/release-signing-key.pem')
        command.add_argument('--trusted-key', type=path, default=Path.home() / '.ame-keys/release-trusted-key.pem')
        command.add_argument('--dev-signing', action='store_true', help='use the development bundle identity')
        default_key = Path.home() / '.ame-keys/anne-avb-key.pem'
        if not default_key.is_file():
            default_key = ROOT / 'out/anne-native-build/dev-avb-key.pem'
        command.add_argument('--avb-key', type=path, default=default_key)
        command.add_argument('--avbtool', type=path, default=ROOT / 'out/host-tools/avb/avbtool.py')
        command.add_argument('--backups', type=path, default=ROOT / 'out/anne-device-private/boot-backups')
        command.add_argument('--output', type=path, help='fresh package directory')

    def run_options(command):
        command.add_argument('--firmware', type=path, default=FIRMWARE)
        command.add_argument('--serial', help='require this handset serial after BootROM upload')
        command.add_argument('--wait', type=nonnegative, default=0, help='BootROM wait in seconds; 0 waits until it appears')
        command.add_argument('--backup', action='store_true', help='also save the kernel being replaced')

    for name, help_text in [('build', 'build all Huawei components and package; no phone access'),
                            ('run', 'flash and boot the recorded build through BootROM; no building'),
                            ('build-run', 'build everything, then flash and boot through BootROM')]:
        command = commands.add_parser(name, help=help_text)
        command.add_argument('--dry-run', action='store_true', help='show build commands or verify run inputs; no USB')
        if name != 'run':
            build_options(command)
        if name != 'build':
            run_options(command)
        if name == 'run':
            command.add_argument('--package', type=path, help='select this exact package instead of the recorded build')
    console = commands.add_parser('console', help='open the raw Ame serial terminal')
    console.add_argument('--port', help='serial device; default auto-detect')
    console.add_argument('--send', help='send one shell command and capture its output')
    console.add_argument('--read', type=positive, default=3, help='capture seconds after --send')
    return result


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    # Private container worker, kept out of the public command list.
    if argv and argv[0] == '_guest':
        worker = argparse.ArgumentParser(description='Internal Linux guest builder')
        worker.add_argument('--linux-source', required=True, type=path)
        worker.add_argument('--output', required=True, type=path)
        worker.add_argument('--export', type=path)
        worker.add_argument('--jobs', type=positive, default=4)
        args = worker.parse_args(argv[1:])
        guest_worker(args.linux_source, args.output, args.jobs)
        if args.export:
            export_guest(args.output, args.export)
        return 0
    args = parser().parse_args(argv)
    try:
        if args.command == 'build':
            build(args)
        elif args.command == 'run':
            run(args)
        elif args.command == 'build-run':
            run(args, build(args))
        elif args.command == 'console':
            command = [sys.executable, TOOLS / 'usb-input.py']
            if args.port:
                command += ['--serial', args.port]
            command.append('console')
            if args.send is not None:
                command += ['--send', args.send, '--enter', '--read', str(args.read)]
            execute(command)
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        detail = error.stderr.decode(errors='replace') if isinstance(getattr(error, 'stderr', None), bytes) else ''
        print('huawei: ' + str(error) + ('\n' + detail if detail else ''), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
