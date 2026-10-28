"""Keep native graphics and the embedded prebuilt renderer in the same build."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('flash_latest', ROOT / 'scripts/anne/flash-latest.py')
flash = importlib.util.module_from_spec(spec)
spec.loader.exec_module(flash)


class RendererRefresh(unittest.TestCase):
    def test_rebuilds_renderer_and_manifest_before_reconfiguring(self):
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp)
            artifacts = build / 'guest'
            artifacts.mkdir()
            (build / 'CMakeCache.txt').write_text(
                f'DANGO_LINUX_ARTIFACT_DIR:PATH={artifacts}\n'
                'DANGO_BUILD_LINUX_ARTIFACTS:BOOL=FALSE\n')
            (artifacts / 'manifest.toml').write_text(
                'name="phone"\nprofile="panfrost"\nrenderer="gles"\n'
                'mali_hardware="1"\nguest="minimal"\n'
                'bootargs="console=ttyAMA0 test=1"\nkernel_load="0x40200000"\n')
            with patch.object(flash, 'run') as run:
                flash.refresh_renderer(build)
            self.assertEqual(len(run.call_args_list), 3)
            first, second, third = run.call_args_list
            self.assertEqual(first.args[0], ['scripts/linux/build-panfrost-renderer.sh'])
            self.assertEqual(second.args[0], ['scripts/linux/build-manifest.sh'])
            env = second.kwargs['env']
            self.assertEqual(env['DANGO_LINUX_OUT'], str(artifacts))
            self.assertEqual(env['DANGO_PANFROST_HARDWARE'], '1')
            self.assertEqual(env['DANGO_LINUX_BOOTARGS'], 'console=ttyAMA0 test=1')
            self.assertEqual(env['DANGO_LINUX_KERNEL_LOAD'], '0x40200000')
            self.assertEqual(third.args[0], ['cmake', '-S', ROOT, '-B', build])

    def test_leaves_other_prebuilt_guests_alone(self):
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp)
            (build / 'CMakeCache.txt').write_text(f'DANGO_LINUX_ARTIFACT_DIR:PATH={build}\n')
            (build / 'manifest.toml').write_text('profile="hello"\n')
            with patch.object(flash, 'run') as run:
                flash.refresh_renderer(build)
            run.assert_not_called()
