#!/usr/bin/env python3
"""Reject wide comparisons surviving in compiled indexed-gather NIR dumps.

Feed MIDGARD_MESA_DEBUG=shaders,internal recording output. This checks compiler
output, without claiming execution: branch condition scheduling otherwise
silently truncates these operands to 32 bits on the scalar unit.
"""
import re
import sys
from pathlib import Path


def check(text):
    shaders = 0
    for section in text.split('shader: ')[1:]:
        if not re.search(r'^name: panvk-index-expand-', section, re.M):
            continue
        shaders += 1
        # The NIR dump precedes MIR's first block. Track input widths rather
        # than the comparison result, which is always a 32-bit boolean.
        nir = section.split('\nblock0:', 1)[0]
        widths = {}
        comparisons = 0
        for line in nir.splitlines():
            definition = re.match(r'\s*(\d+)(?:x\d+)?\s+%(\d+)\s*=', line)
            if definition:
                widths[definition[2]] = int(definition[1])
            comparison = re.search(r'= (?:u(?:lt|ge)|i(?:lt|ge|eq|ne))(?:32)? (.*)', line)
            if comparison:
                comparisons += 1
                operands = re.findall(r'%(\d+)', comparison[1])
                if any(widths.get(operand) == 64 for operand in operands):
                    raise ValueError('64-bit index guard reaches scalar scheduling: ' + line.strip())
        if not comparisons:
            raise ValueError('index shader has no comparisons')
    if not shaders:
        raise ValueError('no compiled indexed-gather shaders in dump')
    return shaders


if __name__ == '__main__':
    try:
        count = check(Path(sys.argv[1]).read_text(errors='replace'))
    except (ValueError, OSError) as error:
        sys.exit(str(error))
    print(f'Midgard index guards: {count} compiled shaders, no wide scalar comparisons')
