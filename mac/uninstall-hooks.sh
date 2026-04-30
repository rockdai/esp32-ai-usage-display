#!/usr/bin/env bash
# Reverse of install-hooks.sh. Removes only entries whose command path points
# to mac/hooks/*.sh under this repo. Leaves any user entries intact. Removes
# event keys that become empty.

set -euo pipefail

SETTINGS="${HOME}/.claude/settings.json"
while [ $# -gt 0 ]; do
  case "$1" in
    --settings) SETTINGS="$2"; shift 2 ;;
    *) echo "unknown flag: $1" >&2; exit 64 ;;
  esac
done

[ -f "$SETTINGS" ] || { echo "$SETTINGS not found"; exit 0; }

HOOKS_DIR="$(cd "$(dirname "$0")/hooks" && pwd)"

jq --arg dir "$HOOKS_DIR" '
  if .hooks then
    .hooks |= with_entries(
      .value |= map(select(.command | startswith($dir + "/") | not))
      | select(.value | length > 0)
    )
    | if (.hooks | length) == 0 then del(.hooks) else . end
  else .
  end
' "$SETTINGS" > "$SETTINGS.tmp"
mv "$SETTINGS.tmp" "$SETTINGS"

echo "uninstalled hooks from $SETTINGS"
