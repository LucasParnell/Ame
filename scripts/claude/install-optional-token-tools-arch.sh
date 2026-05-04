#!/usr/bin/env bash
set -euo pipefail
cat <<'MSG'
Optional token-saving tools for Claude Code on Arch.

Recommended: install one command-output compressor only, not both RTK and Squeez at first.

Commands:
  # usage tracker
  npx ccusage@latest

  # RTK command-output compressor + Claude Code hook
  cargo install --git https://github.com/rtk-ai/rtk
  rtk init -g --auto-patch
  rtk init --show

  # OR Squeez command-output compressor
  npm install -g squeez
  squeez

  # Serena semantic code navigation
  uv tool install -p 3.13 serena-agent@latest --prerelease=allow
  serena init

  # Context Mode Claude Code plugin commands; run inside Claude Code:
  /plugin marketplace add mksglu/context-mode
  /plugin install context-mode@context-mode
  /reload-plugins
  /context-mode:ctx-doctor
MSG
