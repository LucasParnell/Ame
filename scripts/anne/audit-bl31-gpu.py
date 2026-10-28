#!/usr/bin/env python3
"""Extract reviewed GPU/DMSS evidence from the privately held ANE BL31.

Read-only, offline, standard-library only. Accepts the decrypted image, never
fuses, wrapping keys or shared-key RAM. Offsets and function annotations apply
only to the pinned image; reject every other image before interpreting bytes.
This is an evidence extractor, not a firmware patcher or an IOMMU detector.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct


SHA256 = "27c06575c5af056fdbedc9385d4bd3c460459f5075ee79dcb65762f71fa5eb85"
LOAD = 0x35E00000
# GOT slots used by access_register_init at file offsets 0xb860..0xb930.
# Labels are manual disassembly findings, not inferred from function names.
CALLBACKS = (
    (1, 0x1B960, "DDR statistics enable/disable"),
    (2, 0x1B940, "DDR statistics copy to constrained shared buffer"),
    (3, 0x1B8B0, "DDR controller status copy to caller buffer"),
    (4, 0x1B7F0, "DDR controller register writes at 0xfffc9270/0xfffc9274"),
    (6, 0x1B908, "DDR controller command and completion polling"),
    (7, 0x1B8E0, "clock control or constrained register read-modify-write"),
    (8, 0x1B928, "DDR region protection configuration"),
    (10, 0x1B830, "DDR region protection cleanup"),
    (14, 0x1B880, "EVS secure DDR protection enable"),
    (15, 0x1B8A0, "EVS secure DDR protection cleanup"),
)


def audit(raw):
    digest = hashlib.sha256(raw).hexdigest()
    if digest != SHA256:
        raise ValueError("unreviewed BL31 image: SHA-256 does not match")

    def pointer(offset):
        address = struct.unpack_from("<Q", raw, offset)[0]
        relative = address - LOAD
        if not 0 <= relative < len(raw):
            raise ValueError(f"out-of-image pointer at {offset:#x}")
        return relative

    def string(offset):
        end = raw.index(b"\0", offset)
        return raw[offset:end].decode("ascii")

    table = pointer(0x1B870)
    gpu_masters = []
    for index in range(61):
        record = table + index * 24
        name = string(pointer(record))
        if name != "GPU0/GPU1":
            continue
        master, classification = struct.unpack_from("<II", raw, record + 16)
        gpu_masters.append({
            "master_id": hex(master),
            "name": name,
            "traffic": string(pointer(record + 8)),
            "fault_classification": classification,
            "record_file_offset": hex(record),
        })
    counts = struct.unpack_from("<7I", raw, pointer(0x1B918))
    return {
        "image_sha256": digest,
        "image_bytes": len(raw),
        "load_address": hex(LOAD),
        "gpu_masters": gpu_masters,
        "master_lookup_file_offset": "0x4a44",
        "master_classification_is_asi_port": False,
        "dmss_security_region_counts": list(counts),
        "access_register": {
            "smc": "0xc500aa01",
            "cookie_base": "0x55bbcce0",
            "dispatcher_file_offset": "0xb768",
            "registration_file_offset": "0xb860",
            "callbacks": [{
                "index": index,
                "file_offset": hex(pointer(slot)),
                "reviewed_behavior": label,
            } for index, slot, label in CALLBACKS],
            "index_7_rmw": {
                "subcommand": 2,
                "shared_request_bytes": 12,
                "aligned_target_min": "0xe8600000",
                "aligned_target_max": "0xe867fffc",
                "includes_pctrl_gpu_master_ids": False,
            },
        },
        "conclusion": "No GPU external translation route established by these findings.",
        "scope": "Pinned-image static evidence; no hardware route or containment test.",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path, help="decrypted ANE trustfirmware image")
    args = parser.parse_args()
    try:
        result = audit(args.image.read_bytes())
    except (OSError, ValueError, struct.error) as exc:
        parser.exit(1, f"audit: {exc}\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
