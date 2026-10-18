from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class DiskCache(unittest.TestCase):
    def test_partial_write_invalidation_and_late_reply_quarantine(self):
        with tempfile.TemporaryDirectory(prefix='ame-fs-cache-') as directory:
            probe=Path(directory)/'probe'
            command=['clang++','-std=c++23','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',
                     '-I'+str(ROOT/'tests/mochi_host'),'-I'+str(ROOT/'userland/lib/include'),
                     '-I'+str(ROOT/'shared/include'),'-I'+str(ROOT/'lib/include'),
                     str(ROOT/'tests/fs_disk_cache_probe.cc'),str(ROOT/'userland/fs/store.cc'),
                     str(ROOT/'userland/fs/fs_io.cc'),str(ROOT/'lib/libk/printf.cc'),'-o',str(probe)]
            result=subprocess.run(command,capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(probe)],capture_output=True,text=True,timeout=20)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
if __name__=='__main__':unittest.main()
