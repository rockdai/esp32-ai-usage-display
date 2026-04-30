#!/usr/bin/env bash
# Idempotently merge Claude Code hooks into ~/.claude/settings.json so the
# attention scripts fire on UserPromptSubmit / Stop / Notification / SessionEnd.
#
# Usage:
#   mac/install-hooks.sh                          # writes ~/.claude/settings.json
#   mac/install-hooks.sh --settings PATH          # for testing
#
# Each event already has any user-supplied entries left in place; we add ours
# only if the same command path is not already present.

set -euo pipefail

SETTINGS="${HOME}/.claude/settings.json"
while [ $# -gt 0 ]; do
  case "$1" in
    --settings) SETTINGS="$2"; shift 2 ;;
    *) echo "unknown flag: $1" >&2; exit 64 ;;
  esac
done

HOOKS_DIR="$(cd "$(dirname "$0")/hooks" && pwd)"

mkdir -p "$(dirname "$SETTINGS")"
[ -f "$SETTINGS" ] || echo '{}' > "$SETTINGS"

# Build a jq filter that, for each event, appends our entry iff not already present.
jq --arg ups   "$HOOKS_DIR/user-prompt-submit.sh" \
   --arg stp   "$HOOKS_DIR/stop.sh" \
   --arg ntf   "$HOOKS_DIR/notification.sh" \
   --arg send  "$HOOKS_DIR/session-end.sh" \
'
def add_hook(event; cmd):
  .hooks //= {}
  | .hooks[event] //= []
  | if (.hooks[event] | map(.command) | index(cmd)) then .
    else .hooks[event] += [{type:"command", command:cmd}]
    end;

add_hook("UserPromptSubmit"; $ups)
| add_hook("Stop"; $stp)
| add_hook("Notification"; $ntf)
| add_hook("SessionEnd"; $send)
' "$SETTINGS" > "$SETTINGS.tmp"
mv "$SETTINGS.tmp" "$SETTINGS"

echo "installed hooks into $SETTINGS"
