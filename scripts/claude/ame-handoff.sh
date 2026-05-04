#!/usr/bin/env bash
set -euo pipefail
cd "$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
note="${*:-}"
cat > HANDOFF.md <<EOF2
# Ame handoff

## Goal
${note:-TODO: describe current goal}

## Relevant scope
- CMake/scripts/kernel/userland/lib/boot only.
- Do not load build, out, .git, linux-artifacts, docs, zips, logs, specs.

## Files touched
$(git diff --name-only 2>/dev/null | sed 's/^/- /' || true)

## Current git status
\`\`\`
$(git status --short 2>/dev/null || true)
\`\`\`

## Next command
\`\`\`bash
scripts/claude/ame-build.sh
\`\`\`

## Notes
- TODO: exact error/hypothesis if any.
EOF2
printf 'wrote HANDOFF.md\n'
