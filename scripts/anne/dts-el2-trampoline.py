#!/usr/bin/env python3
"""Build and emulate an offline DTS-resident EL2 handoff trampoline.

The generated HSDT candidate is an analysis artifact. It adds a 24-byte
AArch64 stub to the selected FDT, then extends the modem pinctrl write list to
replace four handoff instructions with a register-indirect call. The stub
replays the displaced handoff operations (requesting EL2h), restores x16/x17
and SP, and returns to the original continuation. The selected row uses the
loader's verify_len == 0 acceptance path; this is not a cryptographic proof.
No device transport or flash operation is implemented here.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DTS_MODULE_PATH = ROOT / "scripts/anne/dts-el2-payload.py"
DEFAULT_STOCK = ROOT / "boards/huawei-anne/stock-backups/anne-dts-backup.img"
DEFAULT_FASTBOOT = ROOT / "out/anne-device-private/fastboot-firmware-dump-20261102T214340Z/fastboot.img"
STOCK_DTS_SHA256 = "269fd6ddc8892470916761b373f2063aaecd4bdfa8a503946dbea76fce37ccad"
STOCK_FASTBOOT_SHA256 = "16e307a5d538a76803d2de3a5fd143fb0c61f4aba7a295b5eaa6ae957d7bee52"

DTB_BUFFER_BASE = 0x2E400000
DTB_CAP = 0x200000
PATCH_SITE = 0x10B634
PINCTRL_PARSER = 0x104578
PATCH_RECORD_OFFSET = 0x1779F634
TRAMPOLINE_NODE = "/modem_pinctrl"
TRAMPOLINE_PROPERTY = "ame,trampoline"

# mov w1,#0x3c9; str x5,[x3,#8]; mov x3,#0x88; str w1,[x0,#0x10];
# ldp x16,x17,[sp],#16; ret
PAYLOAD_WORDS = (0x52807921, 0xF9000465, 0xD2801103,
                 0xB9001001, 0xA8C147F0, 0xD65F03C0)
STP_X16_X17_PRE = 0xA9BF47F0
BLR_X16 = 0xD63F0200
INERT_RECORD = [0, 0, 0xFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF]


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def load_module(path: Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not load helper module {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def fdt_tools() -> tuple[str, str]:
    fdtput, fdtget = shutil.which("fdtput"), shutil.which("fdtget")
    if not fdtput or not fdtget:
        raise SystemExit("fdtput and fdtget from device-tree-compiler are required")
    return fdtput, fdtget


def fdtput_cells(binary: str, dtb: Path, prop: str, cells: list[int]) -> None:
    subprocess.run([binary, "-t", "x", str(dtb), TRAMPOLINE_NODE, prop,
                    *(f"{cell:x}" for cell in cells)], check=True)


def fdtput_bytes(binary: str, dtb: Path, prop: str, data: bytes) -> None:
    subprocess.run([binary, "-t", "bx", str(dtb), TRAMPOLINE_NODE, prop,
                    *(f"{byte:x}" for byte in data)], check=True)


def fdtget_cells(binary: str, dtb: Path, node: str, prop: str) -> list[int]:
    words = subprocess.check_output([binary, "-t", "x", str(dtb), node, prop], text=True)
    return [int(word, 16) for word in words.split()]


def fdt_property_offset(blob: bytes, wanted_node: str, wanted_property: str) -> int:
    if len(blob) < 40:
        raise ValueError("FDT is shorter than its 40-byte header")
    header = struct.unpack_from(">10I", blob, 0)
    magic, total, structure_offset, strings_offset, _reserve_offset = header[:5]
    strings_size, structure_size = header[8], header[9]
    if magic != 0xD00DFEED or total > len(blob):
        raise ValueError("invalid FDT header or totalsize")
    if structure_offset + structure_size > total or strings_offset + strings_size > total:
        raise ValueError("FDT structure or strings block exceeds totalsize")
    strings = blob[strings_offset:strings_offset + strings_size]
    structure = blob[structure_offset:structure_offset + structure_size]
    stack: list[str] = []
    cursor = 0
    while cursor + 4 <= len(structure):
        token = struct.unpack_from(">I", structure, cursor)[0]
        cursor += 4
        if token == 1:  # FDT_BEGIN_NODE
            end = structure.find(b"\0", cursor)
            if end < 0:
                raise ValueError("unterminated FDT node name")
            name = structure[cursor:end].decode("ascii", "strict")
            stack.append(name)
            cursor = (end + 4) & ~3
        elif token == 2:  # FDT_END_NODE
            if not stack:
                raise ValueError("unbalanced FDT_END_NODE")
            stack.pop()
        elif token == 3:  # FDT_PROP
            if cursor + 8 > len(structure):
                raise ValueError("truncated FDT_PROP header")
            length, name_offset = struct.unpack_from(">II", structure, cursor)
            data_start = cursor + 8
            data_end = data_start + length
            if data_end > len(structure) or name_offset >= len(strings):
                raise ValueError("FDT_PROP extent exceeds its block")
            name_end = strings.find(b"\0", name_offset)
            if name_end < 0:
                raise ValueError("unterminated FDT property name")
            name = strings[name_offset:name_end].decode("ascii", "strict")
            node = "/" + "/".join(part for part in stack if part)
            if node == wanted_node and name == wanted_property:
                return structure_offset + data_start
            cursor = (data_end + 3) & ~3
        elif token == 4:  # FDT_NOP
            continue
        elif token == 9:  # FDT_END
            break
        else:
            raise ValueError(f"unknown FDT structure token {token:#x}")
    raise ValueError(f"FDT property {wanted_node}:{wanted_property} not found")


def pinctrl_records(payload_address: int) -> tuple[list[list[int]], list[int]]:
    if not 0 <= payload_address <= 0xFFFFFFFF:
        raise ValueError("payload address does not fit the pinctrl writer's 32-bit value")
    patch_words = [
        STP_X16_X17_PRE,
        0xD2800010 | ((payload_address & 0xFFFF) << 5),
        0xF2A00010 | (((payload_address >> 16) & 0xFFFF) << 5),
        BLR_X16,
    ]
    records = []
    for index, word in enumerate(patch_words):
        records.append([0, 0x0B, PATCH_RECORD_OFFSET + index * 4,
                        word, 0, 0xFFFF, 0xFFFF, 0xFFFF])
    return records, patch_words


def build(args) -> dict:
    dts = load_module(DTS_MODULE_PATH, "ame_dts_payload")
    source = Path(args.stock).read_bytes()
    if sha(source) != STOCK_DTS_SHA256:
        raise SystemExit("source DTS hash does not match the captured stock image")
    table = dts.parse_table(source)
    if not 0 <= args.board_id <= 9999:
        raise SystemExit("board ID must be in the range 0..9999")
    digits = [int(ch) for ch in f"{args.board_id:04d}"]
    key = bytes(digits).hex()
    matches = [row for row in table["rows"] if row["key"] == key]
    if len(matches) != 1:
        raise SystemExit(f"expected one HSDT row for board key {key}, found {len(matches)}")
    row = matches[0]
    if not row["vlen"]:
        raise SystemExit("selected stock row has no proof range to preserve")

    fdtput, fdtget = fdt_tools()
    stock_fdt = dts.inflate_at(source, row)
    if len(stock_fdt) > DTB_CAP:
        raise SystemExit("stock FDT exceeds the loader's 2 MiB destination")
    with tempfile.TemporaryDirectory(prefix="ame-dts-trampoline-") as temporary:
        dtb_path = Path(temporary) / "candidate.dtb"
        dtb_path.write_bytes(stock_fdt)
        board_cells = fdtget_cells(fdtget, dtb_path, "/", "hisi,boardid")
        if board_cells != digits:
            raise SystemExit(f"FDT board ID {board_cells} does not match selected row {digits}")

        counts = fdtget_cells(fdtget, dtb_path, TRAMPOLINE_NODE, "pinctrl-num")
        arrays = {f"pinctrl-{i}": fdtget_cells(fdtget, dtb_path, TRAMPOLINE_NODE,
                                                f"pinctrl-{i}") for i in range(3)}
        if len(counts) != 3 or any(counts[i] * 8 != len(arrays[f"pinctrl-{i}"])
                                   for i in range(3)):
            raise SystemExit("stock pinctrl counts do not match array lengths")
        # Extend the pinctrl arrays first. Add the code property afterward so
        # later equal-sized record rewrites cannot move its address.
        records, _ = pinctrl_records(DTB_BUFFER_BASE)
        arrays["pinctrl-0"].extend(word for record in records for word in record)
        inert_flat = INERT_RECORD * len(records)
        arrays["pinctrl-1"].extend(inert_flat)
        arrays["pinctrl-2"].extend(inert_flat)
        new_counts = [count + len(records) for count in counts]
        fdtput_cells(fdtput, dtb_path, "pinctrl-num", new_counts)
        for name, values in arrays.items():
            fdtput_cells(fdtput, dtb_path, name, values)

        placeholder = bytes(len(PAYLOAD_WORDS) * 4)
        fdtput_bytes(fdtput, dtb_path, TRAMPOLINE_PROPERTY, placeholder)
        payload_offset = fdt_property_offset(dtb_path.read_bytes(),
                                             TRAMPOLINE_NODE, TRAMPOLINE_PROPERTY)
        payload_address = DTB_BUFFER_BASE + payload_offset
        records, patch_words = pinctrl_records(payload_address)
        payload = struct.pack("<6I", *PAYLOAD_WORDS)
        active_cells = [word for record in records for word in record]
        arrays["pinctrl-0"][-len(active_cells):] = active_cells
        fdtput_cells(fdtput, dtb_path, "pinctrl-0", arrays["pinctrl-0"])
        fdtput_bytes(fdtput, dtb_path, TRAMPOLINE_PROPERTY, payload)

        final_fdt = dtb_path.read_bytes()
        final_offset = fdt_property_offset(final_fdt, TRAMPOLINE_NODE, TRAMPOLINE_PROPERTY)
        if final_offset != payload_offset:
            raise SystemExit("FDT property moved after installing the final payload")
        if final_fdt[final_offset:final_offset + len(payload)] != payload:
            raise SystemExit("FDT property bytes differ from the trampoline")
        final_counts = fdtget_cells(fdtget, dtb_path, TRAMPOLINE_NODE, "pinctrl-num")
        final_arrays = {f"pinctrl-{i}": fdtget_cells(fdtget, dtb_path, TRAMPOLINE_NODE,
                                                      f"pinctrl-{i}") for i in range(3)}
        if final_counts != new_counts or any(
                final_counts[i] * 8 != len(final_arrays[f"pinctrl-{i}"])
                for i in range(3)):
            raise SystemExit("final pinctrl counts do not match array lengths")
        expected_active = [word for record in records for word in record]
        if final_arrays["pinctrl-0"][-len(expected_active):] != expected_active:
            raise SystemExit("four handoff patch records are not at the end of pinctrl-0")
        if any(final_arrays[name][-len(inert_flat):] != inert_flat
               for name in ("pinctrl-1", "pinctrl-2")):
            raise SystemExit("inert records do not preserve equal pinctrl group counts")

    if len(final_fdt) > DTB_CAP:
        raise SystemExit("rebuilt FDT exceeds the loader's 2 MiB destination")
    compressed = dts.gzip_stream(final_fdt)
    stream_offset = (dts.used_extent(source, table) + 0xFFF) & ~0xFFF
    if stream_offset + len(compressed) > len(source):
        raise SystemExit("no unused tail space for the rebuilt gzip stream")
    if any(stream_offset < end and stream_offset + len(compressed) > start
           for start, end in dts.proof_ranges(table)):
        raise SystemExit("rebuilt gzip stream overlaps a stock proof range")
    if any(stream_offset < other["off"] + other["size"] and
           stream_offset + len(compressed) > other["off"]
           for other in table["rows"] if other["size"]):
        raise SystemExit("rebuilt gzip stream overlaps another HSDT data range")

    candidate = bytearray(source)
    candidate[stream_offset:stream_offset + len(compressed)] = compressed
    struct.pack_into("<I", candidate, row["entry_off"] + 0x08, len(compressed))
    struct.pack_into("<I", candidate, row["entry_off"] + 0x0C, 0)
    struct.pack_into("<I", candidate, row["entry_off"] + 0x10, stream_offset)
    candidate = bytes(candidate)

    manifest = {
        "format": "HSDT v1",
        "stock_sha256": sha(source),
        "candidate_sha256": sha(candidate),
        "candidate_bytes": len(candidate),
        "board_id": args.board_id,
        "entry": row["index"],
        "key": key,
        "verify_len": 0,
        "verification": "loader Verify Not Exist path; stock proof bytes preserved but not checked",
        "verify_offset_preserved": row["voff"],
        "stock_proof_bytes_preserved": row["vlen"],
        "stream_offset": stream_offset,
        "stream_bytes": len(compressed),
        "stream_sha256": sha(compressed),
        "fdt_bytes": len(final_fdt),
        "fdt_sha256": sha(final_fdt),
        "payload_node": TRAMPOLINE_NODE,
        "payload_property": TRAMPOLINE_PROPERTY,
        "payload_data_offset": payload_offset,
        "payload_address": payload_address,
        "payload_bytes": len(payload),
        "payload_hex": payload.hex(),
        "payload_sha256": sha(payload),
        "payload_words": [f"0x{word:08x}" for word in PAYLOAD_WORDS],
        "pinctrl_counts": new_counts,
        "pinctrl_patch_records": records,
        "pinctrl_inert_record": INERT_RECORD,
        "patch_site_image_offset": PATCH_SITE,
        "patch_site_runtime_if_base_0x10000000": 0x10000000 + PATCH_SITE,
        "patch_words": [f"0x{word:08x}" for word in patch_words],
        "overwritten_stock_words": ["0x528078a1", "0xf9000465",
                                     "0xd2801103", "0xb9001001"],
        "replayed_handoff_words": [f"0x{word:08x}" for word in PAYLOAD_WORDS[:4]],
        "resume_image_offset": PATCH_SITE + 16,
        "original_entry": {"size": row["size"], "verify_len": row["vlen"],
                           "data_offset": row["off"], "verify_offset": row["voff"]},
    }
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(candidate)
    manifest_path = Path(args.manifest)
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


def verify_candidate(stock_path: Path, image_path: Path, manifest_path: Path) -> dict:
    dts = load_module(DTS_MODULE_PATH, "ame_dts_payload_verify")
    source, candidate = stock_path.read_bytes(), image_path.read_bytes()
    manifest = json.loads(manifest_path.read_text())
    if sha(source) != STOCK_DTS_SHA256 or manifest.get("stock_sha256") != sha(source):
        raise SystemExit("stock DTS hash does not match manifest or pinned backup")
    if sha(candidate) != manifest.get("candidate_sha256"):
        raise SystemExit("candidate image hash does not match manifest")
    source_table, table = dts.parse_table(source), dts.parse_table(candidate)
    entry = manifest.get("entry")
    if table["count"] != source_table["count"] or table["version"] != source_table["version"]:
        raise SystemExit("HSDT version or row count changed")
    if not isinstance(entry, int) or not 0 <= entry < table["count"]:
        raise SystemExit("manifest entry is out of range")
    row, stock_row = table["rows"][entry], source_table["rows"][entry]
    if row["key"] != manifest.get("key") or row["key"] != stock_row["key"]:
        raise SystemExit("selected HSDT key does not match stock and manifest")
    if row["vlen"] != 0 or row["voff"] != stock_row["voff"]:
        raise SystemExit("candidate must preserve verify offset and use verify_len == 0")
    proof_start = stock_row["voff"]
    proof = source[proof_start:proof_start + stock_row["vlen"]]
    if candidate[proof_start:proof_start + stock_row["vlen"]] != proof:
        raise SystemExit("stock proof bytes changed")
    if row["off"] != manifest.get("stream_offset") or row["size"] != manifest.get("stream_bytes"):
        raise SystemExit("HSDT stream controls differ from manifest")
    compressed = candidate[row["off"]:row["off"] + row["size"]]
    if sha(compressed) != manifest.get("stream_sha256"):
        raise SystemExit("compressed stream hash differs from manifest")
    inflated = dts.inflate_at(candidate, row)
    if len(inflated) > DTB_CAP or len(inflated) != manifest.get("fdt_bytes"):
        raise SystemExit("inflated FDT length is invalid")
    if sha(inflated) != manifest.get("fdt_sha256"):
        raise SystemExit("inflated FDT hash differs from manifest")
    prop_offset = fdt_property_offset(inflated, manifest["payload_node"],
                                      manifest["payload_property"])
    payload = bytes.fromhex(manifest["payload_hex"])
    if prop_offset != manifest.get("payload_data_offset"):
        raise SystemExit("trampoline property offset differs from manifest")
    if DTB_BUFFER_BASE + prop_offset != manifest.get("payload_address"):
        raise SystemExit("trampoline runtime address differs from manifest")
    if inflated[prop_offset:prop_offset + len(payload)] != payload:
        raise SystemExit("FDT trampoline bytes differ from manifest")
    if sha(payload) != manifest.get("payload_sha256"):
        raise SystemExit("trampoline hash differs from manifest")

    expected = bytearray(source)
    expected[row["off"]:row["off"] + row["size"]] = compressed
    for offset, value in ((0x08, row["size"]), (0x0C, 0), (0x10, row["off"])):
        struct.pack_into("<I", expected, row["entry_off"] + offset, value)
    if bytes(expected) != candidate:
        raise SystemExit("candidate changes bytes outside its HSDT row and appended stream")

    _, fdtget = fdt_tools()
    with tempfile.TemporaryDirectory(prefix="ame-dts-trampoline-verify-") as temporary:
        dtb_path = Path(temporary) / "candidate.dtb"
        dtb_path.write_bytes(inflated)
        board_cells = fdtget_cells(fdtget, dtb_path, "/", "hisi,boardid")
        if board_cells != [int(ch) for ch in f"{manifest['board_id']:04d}"]:
            raise SystemExit("inflated FDT board ID does not match the selected HSDT row")
        counts = fdtget_cells(fdtget, dtb_path, TRAMPOLINE_NODE, "pinctrl-num")
        arrays = [fdtget_cells(fdtget, dtb_path, TRAMPOLINE_NODE, f"pinctrl-{i}")
                  for i in range(3)]
        if len(counts) != 3 or any(counts[i] * 8 != len(arrays[i]) for i in range(3)):
            raise SystemExit("pinctrl counts do not match their arrays")
        records, words = pinctrl_records(manifest["payload_address"])
        expected_active = [word for record in records for word in record]
        if arrays[0][-len(expected_active):] != expected_active:
            raise SystemExit("pinctrl-0 does not contain the expected call patch records")
        inert_flat = INERT_RECORD * len(records)
        if arrays[1][-len(inert_flat):] != inert_flat or arrays[2][-len(inert_flat):] != inert_flat:
            raise SystemExit("pinctrl group counts are not balanced with inert records")
        if [f"0x{word:08x}" for word in words] != manifest.get("patch_words"):
            raise SystemExit("pinctrl records encode unexpected patch instructions")
    return {"ok": True, "candidate_sha256": sha(candidate), "entry": entry,
            "key": row["key"], "verify_len": row["vlen"],
            "stock_proof_bytes_preserved": stock_row["vlen"],
            "cryptographic_verification": "not performed; loader acceptance skips verification",
            "payload_address": hex(manifest["payload_address"]),
            "payload_bytes": len(payload), "patch_words": manifest["patch_words"],
            "resume_image_offset": hex(manifest["resume_image_offset"])}


def emulate(args) -> dict:
    report = verify_candidate(Path(args.stock), Path(args.image), Path(args.manifest))
    emu_module = load_module(ROOT / "scripts/anne/tools/emulate-fastboot.py",
                             "ame_emulate_fastboot_trampoline")
    import unicorn
    from unicorn import UC_HOOK_CODE
    from unicorn.arm64_const import (UC_ARM64_REG_PC, UC_ARM64_REG_X0,
                                     UC_ARM64_REG_X3)
    fastboot = Path(args.fastboot).read_bytes()
    if sha(fastboot) != STOCK_FASTBOOT_SHA256:
        raise SystemExit("fastboot image hash does not match the pinned stock capture")
    dts = load_module(DTS_MODULE_PATH, "ame_dts_payload_emulate")
    candidate = Path(args.image).read_bytes()
    manifest = json.loads(Path(args.manifest).read_text())
    row = dts.parse_table(candidate)["rows"][manifest["entry"]]
    fdt = dts.inflate_at(candidate, row)
    emulator = emu_module.FastbootEmulator(fastboot)
    emulator.map(DTB_BUFFER_BASE, DTB_CAP, "loaded FDT and trampoline",
                 unicorn.UC_PROT_ALL)
    emulator.uc.mem_write(DTB_BUFFER_BASE, fdt)
    patch_words = [int(word, 16) for word in manifest["patch_words"]]
    uc = emulator.uc

    # Execute the shipped pinctrl parser and record decoder. Only the FDT
    # service vtable and the peripheral register windows are synthetic; the
    # parser, record conversion, and write-what-where function stay stock.
    api_base = 0x70001000
    generic_api = api_base + 0x10
    emulator.map(api_base, 0x1000, "synthetic FDT operations", unicorn.UC_PROT_ALL)
    emulator.map(0x3EF44000, 0x4000, "pinctrl parser scratch",
                 unicorn.UC_PROT_READ | unicorn.UC_PROT_WRITE)
    mmio_pages = (0xE896C000, 0xFFF11000)
    for page in mmio_pages:
        emulator.map(page, 0x1000, "synthetic pinctrl MMIO",
                     unicorn.UC_PROT_READ | unicorn.UC_PROT_WRITE)

    ret = struct.pack("<I", 0xD65F03C0)
    for address in (0x111438, 0xAAD20, 0xAE78C, 0x1125A4, 0x111E40):
        uc.mem_write(address, ret)
    # FUN_00110f44 returns this synthetic operations table. The table's
    # unused hardware callbacks point at a harmless RET stub; the checked
    # parser callbacks below read the actual FDT property bytes.
    table_address = emu_module.SCRATCH + 0x100
    entries = [generic_api] * 10
    entries[0] = api_base
    entries[1] = api_base + 4
    entries[5] = api_base + 8
    uc.mem_write(table_address, struct.pack("<10Q", *entries))
    uc.mem_write(api_base, ret * 5)
    uc.mem_write(0x110F44, struct.pack("<3I", 0xD2802000, 0xF2AE0000, 0xD65F03C0))

    property_specs = [
        ("pinctrl-num", 12),
        ("pinctrl-0", manifest["pinctrl_counts"][0] * 8 * 4),
        ("pinctrl-1", manifest["pinctrl_counts"][1] * 8 * 4),
        ("pinctrl-2", manifest["pinctrl_counts"][2] * 8 * 4),
    ]
    property_structs = [DTB_BUFFER_BASE + fdt_property_offset(
        fdt, TRAMPOLINE_NODE, name) - 12 for name, _ in property_specs]
    parser_state = {"getprop_calls": 0, "api_calls": [], "hook_error": None}

    def on_api_code(uc, address, _size, _user):
        if address == api_base:
            # fdt_check_header and the pinctrl writer's operation zero.
            uc.reg_write(UC_ARM64_REG_X0, 0)
            parser_state["api_calls"].append("check-or-noop")
        elif address == api_base + 4:
            uc.reg_write(UC_ARM64_REG_X0, 1)  # modem_pinctrl node offset
            parser_state["api_calls"].append("path-offset")
        elif address == api_base + 8:
            index = parser_state["getprop_calls"]
            parser_state["getprop_calls"] += 1
            if index >= len(property_specs):
                parser_state["hook_error"] = "unexpected FDT getprop call"
                uc.emu_stop()
                return
            name, length = property_specs[index]
            uc.reg_write(UC_ARM64_REG_X0, property_structs[index])
            length_pointer = uc.reg_read(UC_ARM64_REG_X3)
            uc.mem_write(length_pointer, struct.pack("<I", length))
            parser_state["api_calls"].append("getprop:" + name)

    parser_hook = uc.hook_add(UC_HOOK_CODE, on_api_code)
    uc.reg_write(UC_ARM64_REG_X0, DTB_BUFFER_BASE)
    parser_error = None
    try:
        uc.emu_start(PINCTRL_PARSER, emu_module.RETURN,
                     timeout=10_000_000, count=1_000_000)
    except unicorn.UcError as error:
        parser_error = str(error)
    finally:
        uc.hook_del(parser_hook)
    parser_pc = uc.reg_read(UC_ARM64_REG_PC)
    parser_return_value = uc.reg_read(UC_ARM64_REG_X0)
    parser_written_words = list(struct.unpack("<4I", uc.mem_read(PATCH_SITE, 16)))
    expected_patch_words = [int(word, 16) for word in manifest["patch_words"]]
    parser_checks = {
        "returned_normally": parser_error is None and parser_pc == emu_module.RETURN,
        "returned_success": parser_return_value == 0,
        "read_four_fdt_properties": parser_state["getprop_calls"] == 4,
        "parser_wrote_all_four_call_words": parser_written_words == expected_patch_words,
        "no_fake_api_error": parser_state["hook_error"] is None,
    }
    pinctrl_report = {
        "entry": hex(PINCTRL_PARSER),
        "return_pc": hex(parser_pc),
        "return_value": hex(parser_return_value),
        "error": parser_error,
        "api_calls": parser_state["api_calls"],
        "patch_words_after_parser": [f"0x{word:08x}" for word in parser_written_words],
        "checks": parser_checks,
        "passed": all(parser_checks.values()),
        "modeled_mmio_pages": [hex(page) for page in mmio_pages],
        "scope": "stock parser/record writer with synthetic FDT API and MMIO callbacks",
    }
    if not pinctrl_report["passed"]:
        raise SystemExit(json.dumps({"pinctrl_parser": pinctrl_report}, indent=2))

    x0, x3 = emu_module.SCRATCH, emu_module.SCRATCH + 0x100
    x5 = 0x1122334455667788
    x16_before, x17_before = 0x1111222233334444, 0x5555666677778888
    sp_before = emu_module.STACK + emu_module.STACK_SIZE - 0x100
    uc.reg_write(emu_module.REGISTERS["x0"], x0)
    uc.reg_write(emu_module.REGISTERS["x3"], x3)
    uc.reg_write(emu_module.REGISTERS["x5"], x5)
    uc.reg_write(emu_module.REGISTERS["x16"], x16_before)
    uc.reg_write(emu_module.REGISTERS["x17"], x17_before)
    uc.reg_write(emu_module.REGISTERS["sp"], sp_before)
    uc.mem_write(x0 + 0x10, bytes(8))
    uc.mem_write(x3 + 8, bytes(8))

    execution = emulator.run(PATCH_SITE, until=PATCH_SITE + 16,
                             count=64, timeout_ms=1000)
    recent = execution.get("recent_instructions", [])
    executed_pcs = [int(item["pc"], 16) for item in recent]
    payload_address = manifest["payload_address"]
    expected_payload_pcs = [payload_address + offset for offset in range(0, 24, 4)]
    registers = execution["registers"]
    memory_x3 = struct.unpack("<Q", uc.mem_read(x3 + 8, 8))[0]
    memory_x0 = struct.unpack("<I", uc.mem_read(x0 + 0x10, 4))[0]
    checks = {
        "stopped_at_original_continuation": (execution["stop_reason"] == "until" and
                                              int(registers["pc"], 16) == PATCH_SITE + 16),
        "all_payload_instructions_executed": all(pc in executed_pcs for pc in expected_payload_pcs),
        "requested_spsr_written": memory_x0 == 0x3C9,
        "displaced_store_replayed": memory_x3 == x5,
        "x3_displaced_move_replayed": int(registers["x3"], 16) == 0x88,
        "stack_restored": int(registers["sp"], 16) == sp_before,
        "x16_restored": int(registers["x16"], 16) == x16_before,
        "x17_restored": int(registers["x17"], 16) == x17_before,
        "link_register_returns_to_continuation":
            int(registers["x30"], 16) == PATCH_SITE + 16,
        "no_emulator_fault": execution.get("stop_reason") == "until" and
                              execution.get("memory_diagnostics", {}).get("invalid_fetches") == 0 and
                              execution.get("memory_diagnostics", {}).get("invalid_reads") == 0 and
                              execution.get("memory_diagnostics", {}).get("invalid_writes") == 0,
    }
    result = {"verification": report, "pinctrl_parser": pinctrl_report,
              "focused_emulation": {
        "fastboot_sha256": sha(fastboot),
        "entry": hex(PATCH_SITE),
        "payload_address": hex(payload_address),
        "return_continuation": hex(PATCH_SITE + 16),
        "execution": execution,
        "observed_memory": {"x3_plus_8": hex(memory_x3),
                            "x0_plus_0x10": hex(memory_x0)},
        "checks": checks,
        "passed": all(checks.values()),
        "scope": "stock pinctrl parser plus focused handoff-site Unicorn replay; synthetic FDT/MMIO services; no cache maintenance or BL31 model",
    }}
    if not all(checks.values()):
        raise SystemExit(json.dumps(result, indent=2))
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    build_parser = sub.add_parser("build")
    build_parser.add_argument("--stock", type=Path, default=DEFAULT_STOCK)
    build_parser.add_argument("--board-id", type=int, default=4970)
    build_parser.add_argument("--out", type=Path, required=True)
    build_parser.add_argument("--manifest", type=Path, required=True)
    build_parser.set_defaults(func=build)
    verify_parser = sub.add_parser("verify")
    verify_parser.add_argument("--stock", type=Path, default=DEFAULT_STOCK)
    verify_parser.add_argument("--image", type=Path, required=True)
    verify_parser.add_argument("--manifest", type=Path, required=True)
    verify_parser.set_defaults(func=lambda args: verify_candidate(
        args.stock, args.image, args.manifest))
    emulate_parser = sub.add_parser("emulate")
    emulate_parser.add_argument("--stock", type=Path, default=DEFAULT_STOCK)
    emulate_parser.add_argument("--image", type=Path, required=True)
    emulate_parser.add_argument("--manifest", type=Path, required=True)
    emulate_parser.add_argument("--fastboot", type=Path, default=DEFAULT_FASTBOOT)
    emulate_parser.set_defaults(func=emulate)
    args = parser.parse_args()
    result = args.func(args)
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
