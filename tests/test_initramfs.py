import gzip
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'scripts/linux' / filename)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


writer = load('initramfs_writer', 'mkinitramfs.py')
checker = load('initramfs_checker', 'check-initramfs.py')


class Initramfs(unittest.TestCase):
    def test_rootfs_and_plain_archive_use_same_contract(self):
        with tempfile.TemporaryDirectory() as work:
            root = Path(work)
            agent = root / 'agent'
            agent.write_bytes(b'guest ELF fixture')
            rootfs = root / 'rootfs'
            (rootfs / 'usr/lib').mkdir(parents=True)
            (rootfs / 'usr/lib/library.so').write_bytes(b'library fixture')
            (rootfs / 'dev').mkdir()
            plain = root / 'initramfs.cpio'
            packed = root / 'initramfs.cpio.gz'
            writer.build(agent, plain, rootfs)
            writer.build(agent, packed, rootfs)
            self.assertEqual(plain.read_bytes(), gzip.decompress(packed.read_bytes()))
            first = packed.read_bytes()
            writer.build(agent, packed, rootfs)
            self.assertEqual(first, packed.read_bytes())
            entries = checker.parse_newc(packed)
            self.assertIn('usr/lib/library.so', entries)
            self.assertEqual(checker.main(['check', str(packed)]), 0)
            writer.build(agent, plain)
            self.assertNotIn('usr/lib/library.so', checker.parse_newc(plain))

    def test_rootfs_cannot_replace_init_or_follow_symlinks(self):
        with tempfile.TemporaryDirectory() as work:
            root = Path(work)
            agent = root / 'agent'
            agent.write_bytes(b'ELF')
            rootfs = root / 'rootfs'
            rootfs.mkdir()
            (rootfs / 'init').write_bytes(b'replacement')
            with self.assertRaises(ValueError):
                writer.build(agent, root / 'archive', rootfs)
            (rootfs / 'init').unlink()
            (rootfs / 'link').symlink_to(agent)
            with self.assertRaises(ValueError):
                writer.build(agent, root / 'archive', rootfs)

    def test_checker_rejects_traversal_duplicates_and_truncation(self):
        with tempfile.TemporaryDirectory() as work:
            archive = Path(work) / 'archive'
            for names in (['../init'], ['/init'], ['init', 'init']):
                with archive.open('wb') as out:
                    for i, name in enumerate(names):
                        writer._write_entry(out, writer.Entry(name, writer.S_IFREG), i)
                    writer._write_entry(out, writer.Entry(writer.TRAILER, writer.S_IFREG), 10)
                with self.assertRaises(SystemExit):
                    checker.parse_newc(archive)
            archive.write_bytes(b'070701')
            with self.assertRaises(SystemExit):
                checker.parse_newc(archive)


if __name__ == '__main__':
    unittest.main()
