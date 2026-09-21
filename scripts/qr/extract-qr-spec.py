#!/usr/bin/env python3
"""Extract the QR Model-2 spec tables into kernel/assets/qr_tables.json.

Development-only helper: it needs `segno` (a dev dependency) purely as a trusted
transcription source for the ISO/IEC 18004 tables, so no spec number is ever
hand-copied into the kernel. The committed JSON is the source of truth and is
turned into C++ by scripts/gen-qr-tables.py.

usage: extract-qr-spec.py OUTPUT.json
"""
from pathlib import Path
import json
import sys

import segno.consts as consts

LEVELS = ('L', 'M', 'Q', 'H')
# segno's 2-bit EC codes, also used to index the format-information table.
LEVEL_CODE = {'L': 1, 'M': 0, 'Q': 3, 'H': 2}
MAX_VERSION = 40


def main(argv):
    if len(argv) != 2:
        print(f'usage: {argv[0]} OUTPUT.json', file=sys.stderr)
        return 2

    ec = {}
    for version in range(1, MAX_VERSION + 1):
        per_level = {}
        for level in LEVELS:
            groups = consts.ECC[version][LEVEL_CODE[level]]
            per_level[level] = [
                {'blocks': g.num_blocks, 'data': g.num_data,
                 'ec': g.num_total - g.num_data}
                for g in groups
            ]
        ec[str(version)] = per_level

    alignment = {str(v): list(consts.ALIGNMENT_POS[v - 2])
                 for v in range(2, MAX_VERSION + 1)}
    version_info = {str(v): consts.VERSION_INFO[v - 7]
                    for v in range(7, MAX_VERSION + 1)}

    document = {
        'provenance': ('Extracted from segno %s (segno.consts), which transcribes '
                       'ISO/IEC 18004:2015 tables. Regenerate with '
                       'scripts/qr/extract-qr-spec.py.' % __import__('segno').__version__),
        'levels': list(LEVELS),
        'level_code': LEVEL_CODE,
        'max_version': MAX_VERSION,
        'format_info': list(consts.FORMAT_INFO),
        'ec': ec,
        'alignment': alignment,
        'version_info': version_info,
    }
    Path(argv[1]).write_text(json.dumps(document, indent=1, sort_keys=True) + '\n')
    print(f'wrote {argv[1]}: {len(ec)} versions, '
          f'{len(document["format_info"])} format-info words, '
          f'{len(alignment)} alignment rows, {len(version_info)} version-info words')
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv))
