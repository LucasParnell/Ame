#!/usr/bin/env python3
import re
import sys
from collections import deque

ERROR_PATTERNS = re.compile(
    r"(error:|fatal error:|undefined reference|ld.lld: error|collect2: error|ninja: build stopped|"
    r"FAILED:|CMake Error|No rule to make target|panic|PANIC|assert|ASSERT|segmentation fault|"
    r"Exception|SError|Data abort|Instruction abort|translation fault|permission fault|fault|backtrace|traceback)",
    re.IGNORECASE,
)
KEEP_PATTERNS = re.compile(
    r"(^FAILED:|^\[[0-9]+/[0-9]+\]|error:|fatal error:|undefined reference|ld.lld: error|"
    r"CMake Error|ninja:|panic|PANIC|assert|ASSERT|fault|abort|backtrace)",
    re.IGNORECASE,
)

before = deque(maxlen=4)
after = 0
kept = []
count = 0
max_lines = int(sys.argv[1]) if len(sys.argv) > 1 else 220

for raw in sys.stdin:
    count += 1
    line = raw.rstrip("\n")
    if ERROR_PATTERNS.search(line):
        while before:
            b = before.popleft()
            if b not in kept:
                kept.append(b)
        kept.append(line)
        after = 10
    elif after > 0 or KEEP_PATTERNS.search(line):
        kept.append(line)
        after -= 1
    else:
        before.append(line)

if kept:
    print("--- filtered Ame build/run output ---")
    for line in kept[-max_lines:]:
        print(line[:500])
    if len(kept) > max_lines:
        print(f"--- truncated: kept last {max_lines} of {len(kept)} relevant lines ---")
else:
    print(f"ok/no critical errors detected in {count} lines")
