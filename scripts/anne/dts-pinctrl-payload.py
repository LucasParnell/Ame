#!/usr/bin/env python3
"""Build and verify an offline DTS modem-pinctrl candidate.

The selected HSDT entry is derived from an explicit four-digit board ID. The
builder leaves the stock FDT intact except for the pinctrl records, stores the
new gzip stream in unused tail space, and updates the selected entry's unsigned
size/offset fields. It sets verify_len to zero, which the captured loader treats
as "Verify Not Exist" success. This is a proof-skip candidate and makes no
cryptographic-verification claim. No device access is performed here.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DTS_MODULE = ROOT / "scripts/anne/dts-el2-payload.py"
STOCK_SHA256 = "269fd6ddc8892470916761b373f2063aaecd4bdfa8a503946dbea76fce37ccad"
FASTBOOT_PATH = ROOT / "out/anne-device-private/fastboot-firmware-dump-20261102T214340Z/fastboot.img"
FASTBOOT_SHA256 = "16e307a5d538a76803d2de3a5fd143fb0c61f4aba7a295b5eaa6ae957d7bee52"
PATCH_RECORD = [0, 0x0b, 0x1779f634, 0x52807921, 0, 0xffff, 0xffff, 0xffff]
INERT_RECORD = [0, 0, 0xfff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff]
PATCH_SITE = 0x10b634
PATCH_WORD = 0x52807921
PINCTRL_PARSER = 0x104578
DTB_BUFFER_BASE = 0x2e400000

# This callback table entry is reached by the normal boot-status GUI. Status 2
# indexes the unlocked-warning strings at 0x289e70/0x289e78. The three NOPs are
# an unused executable slot, below the measured BSS clear range. The shim
# returns only for status 2 and branches to the original handler for others.
WARNING_CALLBACK = 0x289f10
WARNING_HANDLER = 0x82c78
WARNING_STATUS = 2
WARNING_STUB = 0x16008
WARNING_STUB_WORDS = (0x7100081f, 0x54366361, 0xd65f03c0)
WARNING_OLD_WORDS = (0xd503201f, 0xd503201f, 0xd503201f)


def warning_memory_writes(fastboot: bytes) -> list[dict]:
    if sha(fastboot) != FASTBOOT_SHA256:
        raise SystemExit("fastboot image hash does not match the captured ANE-LX1 image")
    if struct.unpack_from("<Q", fastboot, WARNING_CALLBACK)[0] != WARNING_HANDLER:
        raise SystemExit("unlocked-warning callback pointer differs from the captured image")
    old_words = struct.unpack_from("<3I", fastboot, WARNING_STUB)
    if old_words != WARNING_OLD_WORDS:
        raise SystemExit("unlocked-warning shim slot is no longer three NOP instructions")

    delta = WARNING_HANDLER - (WARNING_STUB + 4)
    if delta & 3 or not -(1 << 20) <= delta < (1 << 20):
        raise SystemExit("unlocked-warning callback is outside the B.NE range")
    b_ne = 0x54000000 | (((delta >> 2) & 0x7ffff) << 5) | 1
    if b_ne != WARNING_STUB_WORDS[1]:
        raise SystemExit("unexpected unlocked-warning branch encoding")

    writes = []
    for index, (offset, old_word, word) in enumerate(zip(
            (WARNING_STUB + 4 * i for i in range(3)),
            WARNING_OLD_WORDS, WARNING_STUB_WORDS)):
        writes.append({"name": f"warning_shim_{index}", "offset": offset,
                       "old_word": old_word, "word": word})
    writes.append({"name": "unlocked_warning_callback", "offset": WARNING_CALLBACK,
                   "old_word": WARNING_HANDLER, "word": WARNING_STUB})
    return writes


def record_for_write(offset: int, word: int) -> list[int]:
    # The stock pinctrl record for +0x10b634 uses encoded address 0x1779f634.
    # The executed parser subtracts the same fixed mapping bias for every row.
    encoded = PATCH_RECORD[2] + offset - PATCH_SITE
    if not 0 <= encoded <= 0xffffffff or offset & 3:
        raise ValueError(f"pinctrl write target is out of range or unaligned: {offset:#x}")
    record = list(PATCH_RECORD)
    record[2], record[3] = encoded, word
    return record


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def dts_module():
    spec = importlib.util.spec_from_file_location("anne_dts_payload", DTS_MODULE)
    if spec is None or spec.loader is None:
        raise RuntimeError("could not load dts-el2-payload.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_module(path: Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not load helper module {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def tool(name: str) -> str:
    found = shutil.which(name)
    if not found:
        raise SystemExit(f"required offline tool not found: {name}")
    return found


def fdtget(fdtget_bin: str, dtb: Path, node: str, prop: str) -> list[int]:
    out = subprocess.check_output(
        [fdtget_bin, "-t", "x", str(dtb), node, prop], text=True
    )
    return [int(word, 16) for word in out.split()]


def fdtput(fdtput_bin: str, dtb: Path, prop: str, cells: list[int]) -> None:
    subprocess.run(
        [fdtput_bin, "-t", "x", str(dtb), "/modem_pinctrl", prop,
         *(f"{cell:x}" for cell in cells)], check=True
    )


def board_key(board_id: int) -> tuple[str, list[int]]:
    if not 0 <= board_id <= 9999:
        raise SystemExit("board ID must be a four-digit decimal value")
    digits = [int(ch) for ch in f"{board_id:04d}"]
    return bytes(digits).hex(), digits


def load_dtb_properties(fdtget_bin: str, dtb_path: Path) -> tuple[list[int], dict[str, list[int]]]:
    board_id = fdtget(fdtget_bin, dtb_path, "/", "hisi,boardid")
    counts = fdtget(fdtget_bin, dtb_path, "/modem_pinctrl", "pinctrl-num")
    arrays = {name: fdtget(fdtget_bin, dtb_path, "/modem_pinctrl", name)
              for name in ("pinctrl-0", "pinctrl-1", "pinctrl-2")}
    return board_id, {"pinctrl-num": counts, **arrays}


def build(args) -> dict:
    dts = dts_module()
    source_path = Path(args.stock)
    source = source_path.read_bytes()
    if sha(source) != STOCK_SHA256:
        raise SystemExit("stock dts hash does not match captured ANE-LX1 image")
    table = dts.parse_table(source)
    key, board_digits = board_key(args.board_id)
    matches = [row for row in table["rows"] if row["key"] == key]
    if len(matches) != 1:
        raise SystemExit(f"stock HSDT table has {len(matches)} entries for board ID {args.board_id} ({key})")
    row = matches[0]
    if not row["vlen"]:
        raise SystemExit("selected stock HSDT entry has no proof range to preserve")

    skip_unlock_warning = bool(getattr(args, "skip_unlock_warning", False))
    extra_writes = warning_memory_writes(FASTBOOT_PATH.read_bytes()) if skip_unlock_warning else []
    records = [PATCH_RECORD] + [record_for_write(item["offset"], item["word"])
                                for item in extra_writes]

    fdtget_bin, fdtput_bin = tool("fdtget"), tool("fdtput")
    original_dtb = dts.inflate_at(source, row)
    with tempfile.TemporaryDirectory(prefix="ame-dts-pinctrl-") as temporary:
        dtb_path = Path(temporary) / "candidate.dtb"
        dtb_path.write_bytes(original_dtb)
        fdt_board_id, properties = load_dtb_properties(fdtget_bin, dtb_path)
        if fdt_board_id != board_digits:
            raise SystemExit(
                f"selected FDT board ID {fdt_board_id} does not match HSDT key {key}"
            )
        nums = properties["pinctrl-num"]
        arrays = {name: properties[name] for name in ("pinctrl-0", "pinctrl-1", "pinctrl-2")}
        if len(nums) != 3 or any(nums[index] * 8 != len(arrays[f"pinctrl-{index}"])
                                 for index in range(3)):
            raise SystemExit("stock modem_pinctrl counts do not match their array extents")

        arrays["pinctrl-0"].extend(cell for record in records for cell in record)
        inert_records = INERT_RECORD * len(records)
        arrays["pinctrl-1"].extend(inert_records)
        arrays["pinctrl-2"].extend(inert_records)
        new_counts = [count + len(records) for count in nums]
        fdtput(fdtput_bin, dtb_path, "pinctrl-num", new_counts)
        for name, cells in arrays.items():
            fdtput(fdtput_bin, dtb_path, name, cells)

        final_properties_board_id, final_properties = load_dtb_properties(fdtget_bin, dtb_path)
        final_counts = final_properties["pinctrl-num"]
        final_arrays = {name: final_properties[name] for name in arrays}
        if final_counts != new_counts or any(
                len(final_arrays[name]) != final_counts[index] * 8
                for index, name in enumerate(("pinctrl-0", "pinctrl-1", "pinctrl-2"))):
            raise SystemExit("rebuilt FDT counts do not match their array extents")
        if final_properties_board_id != board_digits:
            raise SystemExit("rebuilt FDT changed its stock board ID")
        expected_active = [cell for record in records for cell in record]
        if final_arrays["pinctrl-0"][-len(expected_active):] != expected_active:
            raise SystemExit("pinctrl write records did not remain at the end of pinctrl-0")
        if any(final_arrays[name][-len(inert_records):] != inert_records
               for name in ("pinctrl-1", "pinctrl-2")):
            raise SystemExit("inert layout records did not remain at the ends of groups 1 and 2")
        candidate_dtb = dtb_path.read_bytes()

    compressed = dts.gzip_stream(candidate_dtb)
    stream_offset = (dts.used_extent(source, table) + 0xfff) & ~0xfff
    if stream_offset + len(compressed) > len(source):
        raise SystemExit("no unused tail space for the rebuilt gzip stream")
    for start, end in dts.proof_ranges(table):
        if stream_offset < end and stream_offset + len(compressed) > start:
            raise SystemExit("new gzip stream overlaps a stock verification proof")
    result = bytearray(source)
    result[stream_offset:stream_offset + len(compressed)] = compressed
    struct.pack_into("<I", result, row["entry_off"] + 0x08, len(compressed))
    struct.pack_into("<I", result, row["entry_off"] + 0x0c, 0)  # explicit proof skip
    struct.pack_into("<I", result, row["entry_off"] + 0x10, stream_offset)
    packed = bytes(result)
    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(packed)
    manifest = {
        "format": "HSDT v1",
        "stock_sha256": sha(source),
        "candidate_sha256": sha(packed),
        "candidate_bytes": len(packed),
        "board_id": args.board_id,
        "entry": row["index"],
        "key": key,
        "unsigned_data_fields": {"size": len(compressed), "offset": stream_offset},
        "verify_len": 0,
        "verify_offset_preserved": row["voff"],
        "verification": "skipped by zero verify_len; no cryptographic proof claim",
        "fdt_bytes": len(candidate_dtb),
        "fdt_sha256": sha(candidate_dtb),
        "gzip_bytes": len(compressed),
        "gzip_sha256": sha(compressed),
        "pinctrl_counts": final_counts,
        "pinctrl_record_counts": {name: len(values) // 8 for name, values in final_arrays.items()},
        "patch_record_cells": PATCH_RECORD,
        "pinctrl_write_records": records,
        "skip_unlock_warning": skip_unlock_warning,
        "unlocked_warning_writes": extra_writes,
        "fastboot_sha256": FASTBOOT_SHA256 if skip_unlock_warning else None,
        "inert_layout_record_cells": INERT_RECORD,
        "layout_reason": "parser computes each next array offset from that group's own count; all counts must match to avoid overlap",
        "patch_site_image_offset": f"0x{PATCH_SITE:x}",
        "patch_word": f"0x{PATCH_WORD:08x}",
        "original_entry": {"size": row["size"], "vlen": row["vlen"],
                           "off": row["off"], "voff": row["voff"]},
    }
    manifest_path = Path(args.manifest)
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


def verify(args) -> dict:
    dts = dts_module()
    reference = Path(args.stock).read_bytes()
    candidate = Path(args.image).read_bytes()
    if sha(reference) != STOCK_SHA256:
        raise SystemExit("stock dts hash does not match captured ANE-LX1 image")
    manifest = json.loads(Path(args.manifest).read_text())
    if manifest.get("stock_sha256") != sha(reference):
        raise SystemExit("manifest stock hash does not match reference image")
    if manifest.get("candidate_sha256") != sha(candidate):
        raise SystemExit("candidate hash does not match manifest")
    if manifest.get("candidate_bytes") != len(candidate):
        raise SystemExit("candidate length does not match manifest")
    skip_unlock_warning = bool(manifest.get("skip_unlock_warning", False))
    extra_writes = warning_memory_writes(FASTBOOT_PATH.read_bytes()) if skip_unlock_warning else []
    records = [PATCH_RECORD] + [record_for_write(item["offset"], item["word"])
                                for item in extra_writes]
    if manifest.get("pinctrl_write_records", [PATCH_RECORD]) != records:
        raise SystemExit("manifest pinctrl records do not match the pinned writes")
    if manifest.get("unlocked_warning_writes", []) != extra_writes:
        raise SystemExit("manifest unlocked-warning writes do not match the pinned fastboot image")
    if skip_unlock_warning and manifest.get("fastboot_sha256") != FASTBOOT_SHA256:
        raise SystemExit("manifest fastboot hash does not match the pinned image")
    board_id = manifest.get("board_id")
    if not isinstance(board_id, int):
        raise SystemExit("manifest does not carry an integer board ID")
    key, digits = board_key(board_id)
    entry = manifest.get("entry")
    stock_table = dts.parse_table(reference)
    candidate_table = dts.parse_table(candidate)
    if stock_table["count"] != candidate_table["count"] or stock_table["version"] != candidate_table["version"]:
        raise SystemExit("HSDT version or entry count changed")
    if not isinstance(entry, int) or not 0 <= entry < stock_table["count"]:
        raise SystemExit("manifest entry is outside the stock HSDT table")
    stock_row = stock_table["rows"][entry]
    candidate_row = candidate_table["rows"][entry]
    if stock_row["key"] != key or candidate_row["key"] != key:
        raise SystemExit("manifest board ID does not select its HSDT entry")
    if candidate_row["vlen"] != 0 or candidate_row["voff"] != stock_row["voff"]:
        raise SystemExit("candidate must explicitly skip verify and preserve verify offset")
    if candidate_row["off"] != manifest["unsigned_data_fields"]["offset"]:
        raise SystemExit("candidate data offset does not match manifest")
    if candidate_row["size"] != manifest["unsigned_data_fields"]["size"]:
        raise SystemExit("candidate compressed size does not match manifest")
    if candidate_row["off"] < dts.used_extent(reference, stock_table):
        raise SystemExit("new stream is not in unused tail space")

    proof_start = stock_row["voff"]
    proof_end = proof_start + stock_row["vlen"]
    if candidate[proof_start:proof_end] != reference[proof_start:proof_end]:
        raise SystemExit("selected entry's stock proof bytes changed")
    compressed = candidate[candidate_row["off"]:candidate_row["off"] + candidate_row["size"]]
    if sha(compressed) != manifest.get("gzip_sha256"):
        raise SystemExit("candidate gzip hash does not match manifest")
    try:
        inflated = dts.inflate_at(candidate, candidate_row)
    except ValueError as error:
        raise SystemExit(str(error)) from error
    if len(inflated) > dts.DTB_CAP:
        raise SystemExit("rebuilt FDT exceeds the loader's 2 MiB output cap")
    if sha(inflated) != manifest.get("fdt_sha256") or len(inflated) != manifest.get("fdt_bytes"):
        raise SystemExit("inflated FDT does not match manifest")

    fdtget_bin = tool("fdtget")
    with tempfile.TemporaryDirectory(prefix="ame-dts-verify-") as temporary:
        dtb_path = Path(temporary) / "candidate.dtb"
        dtb_path.write_bytes(inflated)
        board_cells, properties = load_dtb_properties(fdtget_bin, dtb_path)
        if board_cells != digits:
            raise SystemExit("inflated FDT board ID does not match selected HSDT key")
        counts = properties["pinctrl-num"]
        arrays = {name: properties[name] for name in ("pinctrl-0", "pinctrl-1", "pinctrl-2")}
        if len(counts) != 3 or any(counts[index] * 8 != len(arrays[f"pinctrl-{index}"])
                                   for index in range(3)):
            raise SystemExit("pinctrl count and array extents do not agree")
        active_cells = [cell for record in records for cell in record]
        if arrays["pinctrl-0"][-len(active_cells):] != active_cells:
            raise SystemExit("the selected FDT does not end pinctrl-0 with the expected write records")
        inert_cells = INERT_RECORD * len(records)
        if any(arrays[name][-len(inert_cells):] != inert_cells
               for name in ("pinctrl-1", "pinctrl-2")):
            raise SystemExit("the selected FDT does not contain the inert layout records")

    expected = bytearray(reference)
    expected[candidate_row["off"]:candidate_row["off"] + candidate_row["size"]] = compressed
    struct.pack_into("<I", expected, stock_row["entry_off"] + 0x08, candidate_row["size"])
    struct.pack_into("<I", expected, stock_row["entry_off"] + 0x0c, 0)
    struct.pack_into("<I", expected, stock_row["entry_off"] + 0x10, candidate_row["off"])
    if candidate != bytes(expected):
        raise SystemExit("candidate changes bytes beyond selected fields and appended gzip stream")
    return {"ok": True, "candidate_sha256": sha(candidate), "candidate_bytes": len(candidate),
            "board_id": board_id, "entry": entry, "key": key,
            "verify_len": 0, "proof_bytes_preserved": stock_row["vlen"],
            "gzip_sha256": sha(compressed), "fdt_sha256": sha(inflated),
            "fdt_bytes": len(inflated), "pinctrl_counts": counts,
            "skip_unlock_warning": skip_unlock_warning,
            "unlocked_warning_writes": extra_writes,
            "cryptographic_verification": "not performed; loader bypasses verifier because verify_len=0"}


def emulate(args) -> dict:
    """Execute the stock pinctrl parser against the rebuilt FDT in Unicorn."""
    verification = verify(args)
    import unicorn
    from unicorn import UC_HOOK_CODE
    from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_X0, UC_ARM64_REG_X3, UC_ARM64_REG_X30

    emu_path = ROOT / "scripts/anne/tools/emulate-fastboot.py"
    emu = load_module(emu_path, "anne_emulate_fastboot_pinctrl")
    trampoline = load_module(ROOT / "scripts/anne/dts-el2-trampoline.py",
                             "anne_dts_trampoline_fdt_helpers")
    fastboot = FASTBOOT_PATH.read_bytes()
    candidate = Path(args.image).read_bytes()
    manifest = json.loads(Path(args.manifest).read_text())
    dts = dts_module()
    row = dts.parse_table(candidate)["rows"][manifest["entry"]]
    fdt = dts.inflate_at(candidate, row)

    emulator = emu.FastbootEmulator(fastboot)
    uc = emulator.uc
    emulator.map(DTB_BUFFER_BASE, dts.DTB_CAP, "rebuilt FDT", unicorn.UC_PROT_ALL)
    uc.mem_write(DTB_BUFFER_BASE, fdt)

    api_base = 0x70001000
    generic_api = api_base + 0x10
    emulator.map(api_base, 0x1000, "synthetic FDT operations", unicorn.UC_PROT_ALL)
    emulator.map(0x3ef44000, 0x4000, "pinctrl parser scratch",
                 unicorn.UC_PROT_READ | unicorn.UC_PROT_WRITE)
    mmio_pages = (0xe896c000, 0xfff11000)
    for page in mmio_pages:
        emulator.map(page, 0x1000, "synthetic pinctrl MMIO",
                     unicorn.UC_PROT_READ | unicorn.UC_PROT_WRITE)

    ret = struct.pack("<I", 0xd65f03c0)
    for address in (0x111438, 0xaad20, 0xae78c, 0x1125a4, 0x111e40):
        uc.mem_write(address, ret)
    table_address = emu.SCRATCH + 0x100
    entries = [generic_api] * 10
    entries[0], entries[1], entries[5] = api_base, api_base + 4, api_base + 8
    uc.mem_write(table_address, struct.pack("<10Q", *entries))
    uc.mem_write(api_base, ret * 5)
    uc.mem_write(0x110f44, struct.pack("<3I", 0xd2802000, 0xf2ae0000, 0xd65f03c0))

    property_specs = [
        ("pinctrl-num", 12),
        ("pinctrl-0", manifest["pinctrl_counts"][0] * 8 * 4),
        ("pinctrl-1", manifest["pinctrl_counts"][1] * 8 * 4),
        ("pinctrl-2", manifest["pinctrl_counts"][2] * 8 * 4),
    ]
    property_structs = [DTB_BUFFER_BASE + trampoline.fdt_property_offset(
        fdt, "/modem_pinctrl", name) - 12 for name, _ in property_specs]
    state = {"getprop_calls": 0, "api_calls": [], "hook_error": None}

    def on_api_code(machine, address, _size, _user):
        if address == api_base:
            machine.reg_write(UC_ARM64_REG_X0, 0)
            state["api_calls"].append("check-or-noop")
        elif address == api_base + 4:
            machine.reg_write(UC_ARM64_REG_X0, 1)
            state["api_calls"].append("path-offset")
        elif address == api_base + 8:
            index = state["getprop_calls"]
            state["getprop_calls"] += 1
            if index >= len(property_specs):
                state["hook_error"] = "unexpected FDT getprop call"
                machine.emu_stop()
                return
            name, length = property_specs[index]
            machine.reg_write(UC_ARM64_REG_X0, property_structs[index])
            machine.mem_write(machine.reg_read(UC_ARM64_REG_X3), struct.pack("<I", length))
            state["api_calls"].append("getprop:" + name)

    api_hook = uc.hook_add(UC_HOOK_CODE, on_api_code)
    parser_error = None
    try:
        uc.reg_write(UC_ARM64_REG_X0, DTB_BUFFER_BASE)
        uc.emu_start(PINCTRL_PARSER, emu.RETURN, timeout=10_000_000, count=1_000_000)
    except unicorn.UcError as error:
        parser_error = str(error)
    finally:
        uc.hook_del(api_hook)

    parser_pc = uc.reg_read(UC_ARM64_REG_PC)
    parser_result = uc.reg_read(UC_ARM64_REG_X0)
    writes = warning_memory_writes(fastboot) if manifest.get("skip_unlock_warning") else []
    expected = {PATCH_SITE: PATCH_WORD, **{item["offset"]: item["word"] for item in writes}}
    observed = {}
    for address, word in expected.items():
        observed[hex(address)] = struct.unpack("<I", uc.mem_read(address, 4))[0]
    parser_checks = {
        "returned_normally": parser_error is None and parser_pc == emu.RETURN,
        "returned_success": parser_result == 0,
        "read_four_fdt_properties": state["getprop_calls"] == 4,
        "no_fake_api_error": state["hook_error"] is None,
        "all_requested_memory_words_written": all(
            observed[hex(address)] == word for address, word in expected.items()),
    }

    # Execute the newly written shim itself. Status 2 must return without
    # entering the warning renderer; other status values must branch to it.
    shim_results = {}
    shim_hook = uc.hook_add(UC_HOOK_CODE, lambda machine, address, _size, _user:
                            machine.emu_stop() if address == WARNING_HANDLER else None)
    for status in (1, WARNING_STATUS, 3):
        uc.reg_write(UC_ARM64_REG_X0, status)
        uc.reg_write(UC_ARM64_REG_X30, emu.RETURN)
        uc.emu_start(WARNING_STUB, emu.RETURN, count=16)
        pc = uc.reg_read(UC_ARM64_REG_PC)
        shim_results[str(status)] = {
            "pc": hex(pc),
            "skipped_warning": pc == emu.RETURN,
            "entered_original_handler": pc == WARNING_HANDLER,
        }
    uc.hook_del(shim_hook)
    shim_checks = {
        "unlocked_status_returns": shim_results[str(WARNING_STATUS)]["skipped_warning"],
        "status_1_preserved": shim_results["1"]["entered_original_handler"],
        "status_3_preserved": shim_results["3"]["entered_original_handler"],
    }
    result = {
        "verification": verification,
        "fastboot_sha256": sha(fastboot),
        "pinctrl_parser": {
            "entry": hex(PINCTRL_PARSER),
            "return_pc": hex(parser_pc),
            "return_value": hex(parser_result),
            "error": parser_error,
            "api_calls": state["api_calls"],
            "observed_words": {key: hex(value) for key, value in observed.items()},
            "checks": parser_checks,
            "passed": all(parser_checks.values()),
            "modeled_mmio_pages": [hex(page) for page in mmio_pages],
            "scope": "stock pinctrl parser with synthetic FDT API and MMIO services",
        },
        "warning_shim": {"address": hex(WARNING_STUB), "status_results": shim_results,
                         "checks": shim_checks, "passed": all(shim_checks.values())},
    }
    result["passed"] = result["pinctrl_parser"]["passed"] and result["warning_shim"]["passed"]
    if not result["passed"]:
        raise SystemExit(json.dumps(result, indent=2))
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    build_parser = sub.add_parser("build", help="build proof-skip candidate from stock dts")
    build_parser.add_argument("--stock", default=str(ROOT / "boards/huawei-anne/stock-backups/anne-dts-backup.img"))
    build_parser.add_argument("--board-id", type=int, required=True,
                              help="four-digit decimal board ID observed from the handset")
    build_parser.add_argument("--skip-unlock-warning", action="store_true",
                              help="add writes that conditionally suppress only the status-2 unlocked warning")
    build_parser.add_argument("--out", required=True)
    build_parser.add_argument("--manifest", required=True)
    build_parser.set_defaults(func=build)
    verify_parser = sub.add_parser("verify", help="verify candidate structure and manifest offline")
    verify_parser.add_argument("--stock", default=str(ROOT / "boards/huawei-anne/stock-backups/anne-dts-backup.img"))
    verify_parser.add_argument("--image", required=True)
    verify_parser.add_argument("--manifest", required=True)
    verify_parser.set_defaults(func=verify)
    emulate_parser = sub.add_parser("emulate", help="execute the stock pinctrl parser against the candidate")
    emulate_parser.add_argument("--stock", default=str(ROOT / "boards/huawei-anne/stock-backups/anne-dts-backup.img"))
    emulate_parser.add_argument("--image", required=True)
    emulate_parser.add_argument("--manifest", required=True)
    emulate_parser.set_defaults(func=emulate)
    args = parser.parse_args()
    try:
        result = args.func(args)
        print(json.dumps(result, indent=2))
        return 0
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        print(f"dts-pinctrl-payload: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
