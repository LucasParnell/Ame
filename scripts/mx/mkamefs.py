#!/usr/bin/env python3
"""Build and inspect a Midix/Ame FS_SPEC V1 disk image."""

from __future__ import annotations

import argparse
import hashlib
import math
import os
from pathlib import Path
import struct
import sys
import zlib


BLOCK_SIZE = 4096
SECTOR_SIZE = 512
FS_MAGIC = 0xFEED_FACE
FS_VERSION = 1
BTREE_INTERNAL = 1
BTREE_LEAF = 2
BTREE_ENTRIES_PER_NODE = 63
FS_INODE_FILE = 1
FS_INODE_DIR = 2
FS_CLASS_CONTENT_STORE = 1
FS_CLASS_USER_DATA = 2
JOURNAL_START_BLOCK = 2
JOURNAL_BLOCK_COUNT = 256
FIRST_DATA_BLOCK = JOURNAL_START_BLOCK + JOURNAL_BLOCK_COUNT
BITMAP_COVERAGE_BLOCKS = BLOCK_SIZE * 4


def parse_size(value: str) -> int:
    text = value.strip()
    if not text:
        raise ValueError("empty size")
    suffix = text[-1].lower()
    scale = 1
    if suffix in ("k", "m", "g"):
        text = text[:-1]
        scale = {"k": 1024, "m": 1024 * 1024, "g": 1024 * 1024 * 1024}[suffix]
    return int(text, 0) * scale


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFF_FFFF


def pad_block(data: bytes) -> bytes:
    if len(data) > BLOCK_SIZE:
        raise ValueError("block payload too large")
    return data + bytes(BLOCK_SIZE - len(data))


def name_hash(name: str) -> int:
    return crc32(name.encode("utf-8"))


def pack_btree_header(node_type: int, entry_count: int, generation: int,
                      parent_block: int = 0, next_sibling: int = 0) -> bytes:
    return struct.pack("<HHIQQQ32s",
                       node_type,
                       entry_count,
                       0,
                       generation,
                       parent_block,
                       next_sibling,
                       bytes(32))


def pack_internal_entry(max_key: int, child_block: int) -> bytes:
    return struct.pack("<QQ48s", max_key, child_block, bytes(48))


def pack_inode_entry(inode_number: int, inode_type: int, inode_class: int,
                     size: int, root_block: int, nlink: int, mode: int) -> bytes:
    return struct.pack("<IHHQQIIQQ16s",
                       inode_number,
                       inode_type,
                       inode_class,
                       size,
                       root_block,
                       nlink,
                       mode,
                       0,
                       0,
                       bytes(16))


def pack_extent_entry(logical_block: int, phys_block: int,
                      length_blocks: int, generation: int,
                      data_crc32: int, flags: int = 0) -> bytes:
    return struct.pack("<QQQQHHII20s",
                       logical_block,
                       phys_block,
                       length_blocks,
                       generation,
                       flags,
                       0,
                       data_crc32,
                       0,
                       bytes(20))


def pack_dir_entry(parent_inode: int, child_inode: int,
                   child_type: int, name: str) -> tuple[int, bytes]:
    encoded = name.encode("utf-8")
    if len(encoded) == 0 or len(encoded) > 47:
        raise ValueError(f"directory entry name must be 1..47 bytes: {name!r}")
    key = (parent_inode << 32) | name_hash(name)
    return key, struct.pack("<QIBBH48s",
                            key,
                            child_inode,
                            len(encoded),
                            child_type,
                            0,
                            encoded + bytes(48 - len(encoded)))


def pack_phys_ref_entry(phys_block: int, length_blocks: int,
                        refcount: int = 1, flags: int = 0) -> bytes:
    return struct.pack("<QQII40s",
                       phys_block,
                       length_blocks,
                       refcount,
                       flags,
                       bytes(40))


def pack_free_entry(range_start: int, range_end: int, bitmap_block: int) -> bytes:
    return struct.pack("<QQQ40s", range_start, range_end, bitmap_block, bytes(40))


def pack_snapshot_entry() -> bytes:
    return bytes(64)


class FsImage:
    def __init__(self, path: Path, total_blocks: int):
        self.path = path
        self.total_blocks = total_blocks
        self.next_block = FIRST_DATA_BLOCK
        self.allocated: set[int] = set()
        self.file_data_runs: list[tuple[int, int]] = []
        self.f = path.open("w+b")
        self.f.truncate(total_blocks * BLOCK_SIZE)

    def close(self) -> None:
        self.f.flush()
        os.fsync(self.f.fileno())
        self.f.close()

    def alloc_block(self) -> int:
        if self.next_block >= self.total_blocks:
            raise RuntimeError("filesystem image is too small")
        block = self.next_block
        self.next_block += 1
        self.allocated.add(block)
        return block

    def write_block(self, block: int, data: bytes) -> None:
        if block >= self.total_blocks:
            raise RuntimeError(f"write past image end: block {block}")
        self.f.seek(block * BLOCK_SIZE)
        self.f.write(pad_block(data))

    def alloc_and_write_block(self, data: bytes) -> int:
        block = self.alloc_block()
        self.write_block(block, data)
        return block

    def write_file_data(self, src: Path) -> tuple[int, list[tuple[int, bytes]]]:
        entries: list[tuple[int, bytes]] = []
        logical = 0
        run_start = 0
        run_len = 0
        size = src.stat().st_size
        with src.open("rb") as f:
            while True:
                chunk = f.read(BLOCK_SIZE)
                if not chunk:
                    break
                block_data = pad_block(chunk)
                phys = self.alloc_block()
                self.write_block(phys, block_data)
                if run_len == 0:
                    run_start = phys
                    run_len = 1
                elif run_start + run_len == phys:
                    run_len += 1
                else:
                    self.file_data_runs.append((run_start, run_len))
                    run_start = phys
                    run_len = 1
                entries.append((logical,
                                pack_extent_entry(logical, phys, 1, 1,
                                                  crc32(block_data))))
                logical += 1
        if size == 0:
            return 0, []
        if run_len:
            self.file_data_runs.append((run_start, run_len))
        return size, entries

    def build_btree(self, entries: list[tuple[int, bytes]]) -> int:
        entries = sorted(entries, key=lambda item: item[0])
        for _, payload in entries:
            if len(payload) != 64:
                raise ValueError("btree entry payloads must be 64 bytes")

        leaf_groups = list(chunks(entries, BTREE_ENTRIES_PER_NODE)) or [[]]
        leaf_blocks = [self.alloc_block() for _ in leaf_groups]
        current: list[tuple[int, int]] = []
        for i, group in enumerate(leaf_groups):
            block = leaf_blocks[i]
            next_sibling = leaf_blocks[i + 1] if i + 1 < len(leaf_blocks) else 0
            payload = pack_btree_header(BTREE_LEAF, len(group), 1,
                                        next_sibling=next_sibling)
            payload += b"".join(entry for _, entry in group)
            self.write_block(block, payload)
            max_key = group[-1][0] if group else 0
            current.append((max_key, block))

        while len(current) > 1:
            groups = list(chunks(current, BTREE_ENTRIES_PER_NODE))
            parent_blocks = [self.alloc_block() for _ in groups]
            next_level: list[tuple[int, int]] = []
            for block, group in zip(parent_blocks, groups):
                payload = pack_btree_header(BTREE_INTERNAL, len(group), 1)
                payload += b"".join(pack_internal_entry(max_key, child)
                                    for max_key, child in group)
                self.write_block(block, payload)
                next_level.append((group[-1][0], block))
            current = next_level

        return current[0][1]

    def write_free_bitmaps(self, bitmap_ranges: list[tuple[int, int, int]]) -> int:
        free_runs = 0
        in_free_run = False
        for range_start, range_end, bitmap_block in bitmap_ranges:
            bitmap = bytearray(BLOCK_SIZE)
            for block in range(range_start, range_end):
                if block < FIRST_DATA_BLOCK:
                    state = 0b11
                elif block in self.allocated:
                    state = 0b01
                else:
                    state = 0b00
                index = block - range_start
                bitmap[index // 4] |= state << ((index % 4) * 2)
                if state == 0:
                    if not in_free_run:
                        free_runs += 1
                        in_free_run = True
                else:
                    in_free_run = False
            self.write_block(bitmap_block, bytes(bitmap))
        return free_runs


def chunks(items, size: int):
    for i in range(0, len(items), size):
        yield items[i:i + size]


class Node:
    def __init__(self, name: str, inode_type: int, mode: int,
                 src: Path | None = None):
        self.name = name
        self.inode_type = inode_type
        self.mode = mode
        self.src = src
        self.children: list[Node] = []
        self.inode = 0
        self.size = 0
        self.root_block = 0

    def add_dir(self, name: str, mode: int = 0o555) -> "Node":
        child = Node(name, FS_INODE_DIR, mode)
        self.children.append(child)
        return child

    def add_file(self, name: str, src: Path, mode: int = 0o444) -> "Node":
        child = Node(name, FS_INODE_FILE, mode, src)
        self.children.append(child)
        return child


def walk(node: Node):
    yield node
    for child in node.children:
        yield from walk(child)


def assign_inodes(root: Node) -> None:
    for number, node in enumerate(walk(root), start=1):
        node.inode = number


def build_tree(build_dir: Path, include_initrd: bool) -> tuple[Node, str]:
    repo_root = Path(__file__).resolve().parents[2]
    kernel = build_dir / "kernel" / "anko"
    kernel_bin = build_dir / "kernel" / "anko.bin"
    userland_dir = build_dir / "userland"
    manifest = userland_dir / "manifest.v0"
    if not manifest.is_file():
        manifest = repo_root / "userland" / "manifest.v0"
    manifest_json = build_dir / "manifest.json"
    system_bundle = build_dir / "system.mxb"
    initrd = build_dir / "initrd.mxb"
    for required in (kernel, manifest, manifest_json, system_bundle):
        if not required.is_file():
            raise FileNotFoundError(required)
    userland_images = sorted(userland_dir.glob("*.mxi"))
    if not userland_images:
        raise FileNotFoundError(f"no .mxi files in {userland_dir}")
    if include_initrd and not initrd.is_file():
        raise FileNotFoundError(initrd)

    root = Node("", FS_INODE_DIR, 0o555)
    ame = root.add_dir("ame")
    kernel_dir = ame.add_dir("kernel")
    userland = ame.add_dir("userland")
    userland_bin = userland.add_dir("bin")
    meta = ame.add_dir("meta")
    boot = ame.add_dir("boot")

    kernel_dir.add_file("anko", kernel)
    if kernel_bin.is_file():
        kernel_dir.add_file("anko.bin", kernel_bin)
    userland.add_file("manifest.v0", manifest)
    userland.add_file("manifest.json", manifest_json)
    boot.add_file("system.mxb", system_bundle)
    for image in userland_images:
        userland_bin.add_file(image.name, image)

    manifest_lines = [
        "ame-vfs-version 1",
        "filesystem midix-fs-v1",
        "block_size 4096",
        "layout kernel=/ame/kernel userland=/ame/userland",
        f"kernel anko {kernel.stat().st_size} {sha256_file(kernel)}",
    ]
    if kernel_bin.is_file():
        manifest_lines.append(
            f"kernel anko.bin {kernel_bin.stat().st_size} {sha256_file(kernel_bin)}")
    manifest_lines.append(
        f"userland manifest.v0 {manifest.stat().st_size} {sha256_file(manifest)}")
    manifest_lines.append(
        f"userland manifest.json {manifest_json.stat().st_size} {sha256_file(manifest_json)}")
    manifest_lines.append(
        f"boot system.mxb {system_bundle.stat().st_size} {sha256_file(system_bundle)}")
    for image in userland_images:
        manifest_lines.append(
            f"userland bin/{image.name} {image.stat().st_size} {sha256_file(image)}")

    if include_initrd:
        boot.add_file("initrd.mxb", initrd)
        manifest_lines.append(
            f"boot initrd.mxb {initrd.stat().st_size} {sha256_file(initrd)}")

    install_manifest = build_dir / ".ame-install.manifest"
    install_manifest.write_text("\n".join(manifest_lines) + "\n", encoding="utf-8")
    meta.add_file("install.manifest", install_manifest)
    assign_inodes(root)
    return root, str(install_manifest)


def required_bytes(root: Node) -> int:
    total = FIRST_DATA_BLOCK * BLOCK_SIZE
    for node in walk(root):
        if node.src:
            total += node.src.stat().st_size
            total += math.ceil(max(1, node.src.stat().st_size) / BLOCK_SIZE) * 96
        total += BLOCK_SIZE
    total += 16 * 1024 * 1024
    return total


def create_image(args: argparse.Namespace) -> None:
    build_dir = Path(args.build_dir).resolve()
    output = Path(args.output).resolve()
    root, temp_manifest = build_tree(build_dir, args.include_initrd)
    try:
        size = parse_size(args.size)
        need = required_bytes(root)
        if size < need:
            raise RuntimeError(
                f"image size {size} bytes is too small; need at least {need} bytes")
        if size % BLOCK_SIZE:
            size = ((size + BLOCK_SIZE - 1) // BLOCK_SIZE) * BLOCK_SIZE
        total_blocks = size // BLOCK_SIZE
        output.parent.mkdir(parents=True, exist_ok=True)
        image = FsImage(output, total_blocks)
        try:
            bitmap_ranges: list[tuple[int, int, int]] = []
            for start in range(0, total_blocks, BITMAP_COVERAGE_BLOCKS):
                end = min(start + BITMAP_COVERAGE_BLOCKS, total_blocks)
                bitmap_ranges.append((start, end, image.alloc_block()))

            for node in walk(root):
                if node.inode_type != FS_INODE_FILE or not node.src:
                    continue
                node.size, extents = image.write_file_data(node.src)
                node.root_block = image.build_btree(extents)

            for node in walk(root):
                if node.inode_type != FS_INODE_DIR:
                    continue
                entries = [
                    pack_dir_entry(node.inode, child.inode, child.inode_type, child.name)
                    for child in node.children
                ]
                node.root_block = image.build_btree(entries)

            inode_entries = []
            for node in walk(root):
                inode_class = (FS_CLASS_USER_DATA if node.inode_type == FS_INODE_DIR
                               else FS_CLASS_CONTENT_STORE)
                nlink = 1 + len([c for c in node.children
                                 if c.inode_type == FS_INODE_DIR])
                inode_entries.append((
                    node.inode,
                    pack_inode_entry(node.inode,
                                     node.inode_type,
                                     inode_class,
                                     node.size,
                                     node.root_block,
                                     nlink,
                                     node.mode),
                ))
            inode_root = image.build_btree(inode_entries)

            phys_entries = [
                (start, pack_phys_ref_entry(start, length))
                for start, length in image.file_data_runs
            ]
            phys_ref_root = image.build_btree(phys_entries)
            snapshot_root = image.build_btree([])

            free_entries = [
                (start, pack_free_entry(start, end, bitmap_block))
                for start, end, bitmap_block in bitmap_ranges
            ]
            free_root = image.build_btree(free_entries)
            free_extent_count = image.write_free_bitmaps(bitmap_ranges)

            seed = hashlib.sha256()
            seed.update(output.as_posix().encode("utf-8"))
            seed.update(str(total_blocks).encode("ascii"))
            uuid = seed.digest()[:16]
            superblock = struct.pack(
                "<IIQQQQQQQQQQQII16s3976s",
                FS_MAGIC,
                FS_VERSION,
                total_blocks,
                BLOCK_SIZE,
                inode_root,
                free_root,
                phys_ref_root,
                free_extent_count,
                snapshot_root,
                JOURNAL_START_BLOCK,
                JOURNAL_BLOCK_COUNT,
                FIRST_DATA_BLOCK,
                1,
                0,
                0,
                uuid,
                bytes(3976),
            )
            image.write_block(0, superblock)

            checkpoint_prefix = struct.pack(
                "<QQQQQQQ",
                1,
                inode_root,
                free_root,
                phys_ref_root,
                snapshot_root,
                1,
                0,
            )
            checkpoint = checkpoint_prefix + struct.pack("<II", crc32(checkpoint_prefix), 0)
            image.write_block(1, checkpoint)
        finally:
            image.close()

        print(f"installed Ame native filesystem to {output}")
        print("  format:   midix-fs-v1")
        print("  kernel:   /ame/kernel")
        print("  userland: /ame/userland")
    finally:
        try:
            Path(temp_manifest).unlink()
        except FileNotFoundError:
            pass


def inspect_image(args: argparse.Namespace) -> None:
    image = Path(args.image)
    with image.open("rb") as f:
        superblock = f.read(BLOCK_SIZE)
        fields = struct.unpack("<IIQQQQQQQQQQQII16s3976s", superblock)
        magic, version = fields[0], fields[1]
        if magic != FS_MAGIC:
            raise RuntimeError(f"{image} is not a Midix FS image: magic=0x{magic:08x}")
        print(f"format midix-fs-v{version}")
        print(f"blocks {fields[2]}")
        print(f"block_size {fields[3]}")
        print(f"inode_table_root {fields[4]}")
        print(f"extent_bitmap_root {fields[5]}")
        print(f"phys_refcount_root {fields[6]}")
        print(f"free_extent_count {fields[7]}")
        print(f"snapshot_tree_root {fields[8]}")
        print(f"journal_start_block {fields[9]}")
        print(f"journal_block_count {fields[10]}")
        print(f"first_data_block {fields[11]}")
        if args.list:
            print_paths(f, fields[4])


def read_block(f, block: int) -> bytes:
    f.seek(block * BLOCK_SIZE)
    data = f.read(BLOCK_SIZE)
    if len(data) != BLOCK_SIZE:
        raise RuntimeError(f"short read at block {block}")
    return data


def read_btree_entries(f, root_block: int, visited: set[int] | None = None) -> list[bytes]:
    if root_block == 0:
        return []
    if visited is None:
        visited = set()
    if root_block in visited:
        raise RuntimeError(f"btree loop at block {root_block}")
    visited.add(root_block)
    block = read_block(f, root_block)
    node_type, entry_count, _, _, _, _ = struct.unpack("<HHIQQQ32s", block[:64])[:6]
    if entry_count > BTREE_ENTRIES_PER_NODE:
        raise RuntimeError(f"invalid btree entry count {entry_count} at block {root_block}")
    entries = [block[64 + i * 64:128 + i * 64] for i in range(entry_count)]
    if node_type == BTREE_LEAF:
        return entries
    if node_type != BTREE_INTERNAL:
        raise RuntimeError(f"invalid btree node type {node_type} at block {root_block}")
    out: list[bytes] = []
    for entry in entries:
        _, child = struct.unpack("<QQ48s", entry)[0:2]
        out.extend(read_btree_entries(f, child, visited))
    return out


def print_paths(f, inode_root: int) -> None:
    inodes = {}
    for entry in read_btree_entries(f, inode_root):
        inode_number, inode_type, inode_class, size, root_block, nlink, mode, _, _, _ = \
            struct.unpack("<IHHQQIIQQ16s", entry)
        if inode_number:
            inodes[inode_number] = {
                "type": inode_type,
                "class": inode_class,
                "size": size,
                "root": root_block,
                "nlink": nlink,
                "mode": mode,
            }

    def visit(inode: int, path: str) -> None:
        meta = inodes.get(inode)
        if not meta:
            return
        kind = "dir" if meta["type"] == FS_INODE_DIR else "file"
        print(f"{kind} {path or '/'} size={meta['size']} inode={inode}")
        if meta["type"] != FS_INODE_DIR:
            return
        children = []
        for entry in read_btree_entries(f, meta["root"]):
            _, child_inode, name_len, child_type, _, name_raw = \
                struct.unpack("<QIBBH48s", entry)
            if child_inode == 0 or name_len == 0:
                continue
            name = name_raw[:name_len].decode("utf-8", "replace")
            children.append((name, child_inode, child_type))
        for name, child_inode, _ in sorted(children):
            child_path = f"{path}/{name}" if path else f"/{name}"
            visit(child_inode, child_path)

    visit(1, "")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="cmd", required=True)

    create = sub.add_parser("create")
    create.add_argument("--output", required=True)
    create.add_argument("--build-dir", default="build")
    create.add_argument("--size", default="128M")
    create.add_argument("--include-initrd", action="store_true")
    create.set_defaults(func=create_image)

    inspect = sub.add_parser("inspect")
    inspect.add_argument("image")
    inspect.add_argument("--list", action="store_true")
    inspect.set_defaults(func=inspect_image)

    args = parser.parse_args(argv)
    args.func(args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
