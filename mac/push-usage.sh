#!/usr/bin/env bash
#
# push-usage.sh — aggregate Claude Code usage and POST to the ESP32 display.
#
# Sources:
#   1. ccusage `blocks --active --json`        → 5h block window
#   2. ccusage `daily --json`                  → today + weekly token/cost sums
#   3. recursive scan of ~/.claude/projects/   → weekly window started_at
#                                                 (earliest type:"user" event in
#                                                  the trailing 168 h)
#
# Test mode: when --ccusage-fixture-dir is supplied, the script reads
# blocks-active.json / daily.json from that directory instead of running ccusage,
# and reads jsonl files under --claude-dir instead of ~/.claude/projects.
# When --emit-only is set the script prints the JSON payload to stdout and
# exits 0 (no HTTP POST). --now pins the clock for deterministic tests.
#
# Wire schema: see docs/superpowers/specs/2026-04-26-esp32-ai-usage-display-design.md §6.
# IMPORTANT: the schema is a contract with the firmware (Task 12). Do not add
# quota fields (percent, limit_tokens) — by design.

set -euo pipefail

NOW="$(date +%s)"
PLAN_NAME="${PLAN_NAME:-Max 5x}"
CCUSAGE_FIXTURE_DIR=""
CLAUDE_DIR="${HOME}/.claude/projects"
EMIT_ONLY=0

while [ $# -gt 0 ]; do
  case "$1" in
    --emit-only)               EMIT_ONLY=1; shift ;;
    --now)                     NOW="$2";  shift 2 ;;
    --ccusage-fixture-dir)     CCUSAGE_FIXTURE_DIR="$2"; shift 2 ;;
    --claude-dir)              CLAUDE_DIR="$2"; shift 2 ;;
    *) echo "unknown flag: $1" >&2; exit 64 ;;
  esac
done

# ISO-8601 (with optional fractional seconds and trailing "Z") -> integer epoch.
# BSD date can't parse fractional seconds reliably; python3 is always available
# on macOS.
iso_to_epoch() {
  python3 -c 'import sys,datetime; print(int(datetime.datetime.fromisoformat(sys.argv[1].replace("Z","+00:00")).timestamp()))' "$1"
}

ccusage_blocks_json() {
  if [ -n "$CCUSAGE_FIXTURE_DIR" ]; then
    cat "$CCUSAGE_FIXTURE_DIR/blocks-active.json"
  else
    npx ccusage@latest blocks --active --json 2>/dev/null
  fi
}

ccusage_daily_json() {
  if [ -n "$CCUSAGE_FIXTURE_DIR" ]; then
    cat "$CCUSAGE_FIXTURE_DIR/daily.json"
  else
    npx ccusage@latest daily --json 2>/dev/null
  fi
}

# Earliest type:"user" timestamp in the trailing 168 h, scanning .jsonl
# files recursively under $CLAUDE_DIR. Prints an epoch on stdout. If no
# qualifying event exists, falls back to NOW − 168 h (the window floor).
weekly_started_at() {
  local since=$(( NOW - 7*86400 ))
  local min=""
  local ts ep

  # find may exit non-zero (e.g. dir missing); tolerate it.
  while IFS= read -r ts; do
    [ -z "$ts" ] && continue
    ep="$(iso_to_epoch "$ts" 2>/dev/null || true)"
    [ -z "$ep" ] && continue
    if [ "$ep" -lt "$since" ]; then
      continue
    fi
    if [ -z "$min" ] || [ "$ep" -lt "$min" ]; then
      min="$ep"
    fi
  done < <(
    find "$CLAUDE_DIR" -type f -name '*.jsonl' 2>/dev/null \
      | while IFS= read -r f; do
          jq -r 'select(.type=="user") | .timestamp // empty' "$f" 2>/dev/null || true
        done
  )

  echo "${min:-$since}"
}

build_payload() {
  local now="$1"
  local blocks daily wstart today_date

  blocks="$(ccusage_blocks_json)"
  daily="$(ccusage_daily_json)"
  wstart="$(weekly_started_at)"
  today_date="$(date -r "$now" +%Y-%m-%d)"

  jq -nc \
    --argjson now    "$now" \
    --argjson wstart "$wstart" \
    --arg     plan   "$PLAN_NAME" \
    --argjson blocks "$blocks" \
    --argjson daily  "$daily" \
    --arg     today_date "$today_date" \
    '
      # Normalize ISO-8601 ("2026-05-02T15:00:00.000Z") for fromdateiso8601,
      # which only accepts "%Y-%m-%dT%H:%M:%SZ" — strip fractional seconds.
      # Returns 0 for null input.
      def iso_to_epoch:
        if . == null then 0
        else (sub("\\.[0-9]+Z$"; "Z") | fromdateiso8601)
        end;

      # Round a number to 2 decimal places (cents). Avoids FP noise from
      # summing per-day costs (e.g. 9.78 + 13.91 yields 23.689999999999998
      # in IEEE 754). Matching the two-decimal input keeps the wire output
      # stable and easy to assert on.
      def cents: (. * 100 | round) / 100;

      ($blocks.blocks // []) as $bs |
      (if ($bs | length) > 0 then $bs[0] else {} end) as $b |
      ($daily.daily // []) as $d |
      ($wstart | strftime("%Y-%m-%d")) as $wstart_date |
      ($d | map(select(.date >= $wstart_date))) as $win |
      ($d | map(select(.date == $today_date)) | (if length > 0 then .[0] else null end)) as $today |
      {
        ts:   $now,
        plan: $plan,
        block_5h: {
          used_tokens:   ($b.totalTokens // 0),
          started_at:    ($b.startTime  // null | iso_to_epoch),
          resets_at:     ($b.endTime    // null | iso_to_epoch),
          cost_usd:      (($b.costUSD   // 0) | cents),
          messages:      ($b.entries    // 0),
          burn_rate_tpm: ((($b.burnRate // {}).tokensPerMinute // 0) | floor)
        },
        weekly: {
          used_tokens: ($win | map(.totalTokens // 0) | add // 0),
          started_at:  $wstart,
          resets_at:   ($wstart + 7*86400),
          cost_usd:    (($win | map(.totalCost  // 0) | add // 0) | cents),
          messages:    0
          # v2 TODO: derive by counting type:user events from
          # ~/.claude/projects scan with timestamp >= weekly.started_at.
          # The ccusage daily aggregation does not expose message counts.
        },
        today: (
          if $today == null then
            { tokens: 0, messages: 0, cost_usd: 0 }
          else
            { tokens: ($today.totalTokens // 0), messages: 0, cost_usd: (($today.totalCost // 0) | cents) }
          end
        )
      }
    '
}

payload="$(build_payload "$NOW")"

if [ "$EMIT_ONLY" -eq 1 ]; then
  echo "$payload"
  exit 0
fi

# Real-mode POST.
# shellcheck disable=SC1091
. "$(dirname "$0")/secrets.env"
echo "$payload" | curl -fsS --max-time 10 \
  -X POST -H 'Content-Type: application/json' \
  --data @- "http://${HOST}/data"
