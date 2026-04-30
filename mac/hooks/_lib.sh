# mac/hooks/_lib.sh — shared payload + curl helper for Claude Code attention hooks.
#
# Sourced by per-event hook scripts. Caller passes the state literal, e.g.:
#     . "$(dirname "$0")/_lib.sh"
#     post_attention DONE
#
# Reads HOST from ../secrets.env. Reads CLAUDE_PROJECT_DIR / CLAUDE_SESSION_ID
# from env (set by Claude Code when invoking hooks); falls back to $PWD / "".
#
# Hook scripts MUST NOT block Claude on network failures. Curl has a 2 s
# max-time and the helper always returns 0 (|| true).
#
# jq is required (already a dep of mac/push-usage.sh).

post_attention() {
  local state="$1"
  local lib_dir mac_dir
  lib_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  mac_dir="$(cd "$lib_dir/.." && pwd)"

  # shellcheck disable=SC1091
  . "$mac_dir/secrets.env"

  local ts cwd sid payload
  ts="$(date +%s)"
  cwd="${CLAUDE_PROJECT_DIR:-$PWD}"
  sid="${CLAUDE_SESSION_ID:-}"

  payload="$(jq -nc \
    --argjson ts  "$ts" \
    --arg     state "$state" \
    --arg     cwd "$cwd" \
    --arg     sid "$sid" \
    '{ts:$ts, state:$state, cwd:$cwd, session_id:$sid}')"

  printf '%s' "$payload" | \
  curl --max-time 2 -sf \
       -X POST -H 'Content-Type: application/json' \
       --data @- \
       "http://${HOST}/attention" \
       >/dev/null 2>&1 || true
}
