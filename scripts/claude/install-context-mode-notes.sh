#!/usr/bin/env bash
set -euo pipefail
cat <<'MSG'
Run these inside Claude Code from the Ame repo:

/plugin marketplace add mksglu/context-mode
/plugin install context-mode@context-mode
/reload-plugins
/context-mode:ctx-doctor

Then tell Claude:
Use Context Mode for commands/searches that may produce huge output. Never dump raw CMake/Ninja/QEMU logs into context.
MSG
