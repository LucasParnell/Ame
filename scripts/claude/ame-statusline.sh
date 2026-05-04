#!/usr/bin/env bash
# Claude Code pipes status JSON to stdin. Keep this script tiny and local.
input="$(cat)"
if ! command -v jq >/dev/null 2>&1; then
  printf 'Ame | install jq for statusline'
  exit 0
fi
model="$(printf '%s' "$input" | jq -r '.model.display_name // .model.name // "Claude"')"
pct="$(printf '%s' "$input" | jq -r '(.context_window.used_percentage // 0) | floor')"
cost="$(printf '%s' "$input" | jq -r '(.cost.total_cost_usd // 0)')"
repo="$(basename "$(git rev-parse --show-toplevel 2>/dev/null || pwd)")"
branch="$(git branch --show-current 2>/dev/null || true)"
[ -z "$branch" ] && branch="nogit"
bar_width=10
filled=$(( pct * bar_width / 100 ))
empty=$(( bar_width - filled ))
bar=""
if [ "$filled" -gt 0 ]; then bar="$(printf '%*s' "$filled" '' | tr ' ' '#')"; fi
if [ "$empty" -gt 0 ]; then bar="$bar$(printf '%*s' "$empty" '' | tr ' ' '-')"; fi
printf '%s | %s:%s | ctx %s%% [%s] | $%.2f' "$model" "$repo" "$branch" "$pct" "$bar" "$cost"
