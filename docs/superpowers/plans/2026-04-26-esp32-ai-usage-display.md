# ESP32 AI Usage Display — v1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship a working desk display that shows the user's Claude Code 5-hour and weekly quota burn, refreshed every 60 s by a Mac-side launchd job, with token usage as a secondary metric.

**Architecture:** Mac-side `launchd` runs `mac/push-usage.sh` every 60 s; the script computes 5h/weekly aggregates from Claude Code logs (via `ccusage`), POSTs JSON to the ESP32 over the LAN. ESP32 (Arduino + LovyanGFX, built with PlatformIO) advertises `ai-usage-display.local` via mDNS, exposes `POST /data`, stores the latest payload, and redraws the B1 landscape layout each second.

**Tech Stack:**
- Mac: bash, `ccusage` (npm), `jq`, `curl`, `launchd`, `bats-core` (tests)
- ESP32: PlatformIO + Arduino framework, `LovyanGFX`, `ArduinoJson`, `WebServer`, `ESPmDNS`
- Hardware: Waveshare ESP32-S3-RLCD-4.2 (300×400 reflective LCD; rotated to 400×300 landscape)

**Spec:** [`docs/superpowers/specs/2026-04-26-esp32-ai-usage-display-design.md`](../specs/2026-04-26-esp32-ai-usage-display-design.md)

---

## Revision notes (2026-04-26, post-T1/T2)

After the Phase A research, the spec and plan were revised. Tasks below
already reflect the revisions; this section is a guide for engineers reading
older history.

1. **No quota percentages.** Anthropic does not publish the Max-5x token caps;
   computing `tokens / fabricated_cap × 100%` would lie. The display now shows
   absolute `used_tokens` as the dominant figure on each row, and the bar's
   fill represents **time progress through the window**, not quota. See spec
   §1, §6, §7.
2. **Wire schema renamed and reshaped.** `window_5h` → `block_5h`. Fields
   `percent` and `limit_tokens` are gone. New fields: `started_at`, `cost_usd`,
   `messages` (both rows), and `burn_rate_tpm` (5h block only). Today payload
   adds `messages` and `cost_usd`. See spec §6.
3. **Weekly clock approximation.** Claude Code does not persist the weekly
   period start anywhere on disk and `/status` is not invokable from the CLI.
   `push-usage.sh` therefore scans `~/.claude/projects/**/*.jsonl` for the
   earliest `type:"user"` event with `timestamp >= now − 168 h`. Single-machine
   accounts match `/status` exactly; multi-machine accounts may drift up to 7 d.
4. **LCD strategy change.** No upstream LovyanGFX driver exists for the panel
   (Sitronix ST7305, mono, non-linear 4×2 tile framebuffer). Instead of
   subclassing `LGFX_Device`, Task 13 vendors Waveshare's `display_bsp.{h,cpp}`
   for chip init and uses `LGFX_Sprite` + `DisplayPort` to compose 1-bit
   frames. See `docs/superpowers/notes/2026-04-26-lcd-driver.md`.

---

## Phase A — De-risk before writing code

These two tasks resolve the open questions in spec §11. They produce notes in `docs/superpowers/notes/`, not production code.

### Task 1: Validate `ccusage` capability and document Max 5x plan limits

**Files:**
- Create: `docs/superpowers/notes/2026-04-26-ccusage-investigation.md`

- [ ] **Step 1: Install ccusage and run on the user's real Mac account**

```bash
npx ccusage@latest --help
npx ccusage@latest      # default summary
npx ccusage@latest --json
npx ccusage@latest blocks   # 5-hour billing blocks if supported
```

- [ ] **Step 2: Note which fields ccusage exposes**

Record in the notes file: which command surfaces (a) tokens within the trailing 5 h, (b) tokens within the current weekly window, (c) per-day breakdowns, (d) current 5 h block reset time.

- [ ] **Step 3: Determine Max 5x plan token limits**

Sources, in order of preference: (1) Anthropic's published rate-limit docs, (2) the Claude Code `/status` output on the user's machine, (3) empirical observation. Record both the 5 h limit and the weekly limit as integers.

- [ ] **Step 4: Decide aggregation strategy**

If ccusage gives the windows directly, `push-usage.sh` will use them. If not, the script will read the raw daily JSON and aggregate locally. Document the decision in the notes file.

- [ ] **Step 5: Commit**

```bash
git add docs/superpowers/notes/2026-04-26-ccusage-investigation.md
git commit -m "docs: ccusage capability + Max 5x plan limits investigation"
```

### Task 2: Source LovyanGFX panel configuration for the RLCD

**Files:**
- Create: `docs/superpowers/notes/2026-04-26-lcd-driver.md`

- [ ] **Step 1: Locate Waveshare's official Arduino sample**

From <https://docs.waveshare.net/ESP32-S3-RLCD-4.2/>, follow the demo download link. Save the LovyanGFX/TFT panel init snippet (driver class, SPI pins, init sequence, color order, rotation) to the notes file.

- [ ] **Step 2: Identify the panel driver class**

LovyanGFX has classes like `Panel_ILI9341`, `Panel_ST7789`, `Panel_GC9A01`, etc. Determine which matches Waveshare's sample, or whether a custom `Panel_XXX` is required.

- [ ] **Step 3: Record the exact GPIO pinout**

LCD `SCLK`, `MOSI`, `MISO` (if used), `DC`, `CS`, `RST`, `BL` (probably unused on RLCD). Note any voltage/timing quirks.

- [ ] **Step 4: Commit**

```bash
git add docs/superpowers/notes/2026-04-26-lcd-driver.md
git commit -m "docs: LCD driver and pinout for ESP32-S3-RLCD-4.2"
```

---

## Phase B — Mac side (fully testable without the device)

### Task 3: Mac project skeleton

**Files:**
- Create: `mac/secrets.env.example`
- Create: `mac/fixtures/.gitkeep`
- Create: `mac/test/.gitkeep`

- [ ] **Step 1: Create directory layout**

```bash
mkdir -p mac/fixtures mac/test
touch mac/fixtures/.gitkeep mac/test/.gitkeep
```

- [ ] **Step 2: Create secrets template**

Write `mac/secrets.env.example`:

```bash
# Copy to mac/secrets.env (gitignored) and edit.
HOST=ai-usage-display.local
```

- [ ] **Step 3: Verify .gitignore covers `mac/secrets.env`**

Run: `git check-ignore -v mac/secrets.env`
Expected: prints the matching `.gitignore` line. (`mac/secrets.env` should already be ignored from the previous commit.)

- [ ] **Step 4: Commit**

```bash
git add mac/
git commit -m "mac: scaffold directories and secrets template"
```

### Task 4: Install bats-core for shell tests

**Files:**
- Create: `mac/test/test_helper.bash`
- Modify: `README.md` (add a one-liner pointing to `bats` for tests)

- [ ] **Step 1: Install bats-core via Homebrew**

Run: `brew install bats-core`
Expected: bats binary available at `which bats`.

- [ ] **Step 2: Write a smoke test to confirm bats works**

Create `mac/test/smoke.bats`:

```bash
#!/usr/bin/env bats

@test "bats is alive" {
  result="$(echo hello)"
  [ "$result" = "hello" ]
}
```

- [ ] **Step 3: Run the smoke test**

Run: `bats mac/test/smoke.bats`
Expected: `1 test, 0 failures`.

- [ ] **Step 4: Commit**

```bash
git add mac/test/smoke.bats
git commit -m "mac: bats smoke test confirms test runner works"
```

### Task 5: `push-usage.sh` — aggregation core (TDD against fixtures)

**Files:**
- Create: `mac/push-usage.sh`
- Create: `mac/fixtures/sample/blocks-active.json`        (mock ccusage)
- Create: `mac/fixtures/sample/daily.json`                (mock ccusage)
- Create: `mac/fixtures/sample/projects/demo/abc123.jsonl` (mock Claude logs)
- Create: `mac/test/aggregate.bats`

The aggregation orchestrates three data sources: (1) `ccusage blocks --active --json` for the 5h block, (2) `ccusage daily --json` for today and weekly token sums, (3) a recursive scan of `~/.claude/projects/**/*.jsonl` for the weekly window's `started_at`. The script is testable by feeding it fixture directories instead of running ccusage / scanning `~/.claude/`.

- [ ] **Step 1: Write fixtures**

`mac/fixtures/sample/blocks-active.json` (mocks `ccusage blocks --active --json`):

```json
{
  "blocks": [
    {
      "id":         "2026-04-26T15:00:00.000Z",
      "startTime":  "2026-04-26T15:00:00.000Z",
      "endTime":    "2026-04-26T20:00:00.000Z",
      "isActive":   true,
      "isGap":      false,
      "entries":    11,
      "totalTokens":1017862,
      "costUSD":    1.81,
      "burnRate":   { "tokensPerMinute": 82684.1, "costPerHour": 8.81 }
    }
  ]
}
```

`mac/fixtures/sample/daily.json` (mocks `ccusage daily --json`; covers the weekly window 2026-04-19 → 2026-04-26):

```json
{
  "daily": [
    {"date":"2026-04-19","totalTokens":     0,"totalCost":0.00},
    {"date":"2026-04-20","totalTokens":     0,"totalCost":0.00},
    {"date":"2026-04-21","totalTokens":     0,"totalCost":0.00},
    {"date":"2026-04-22","totalTokens":     0,"totalCost":0.00},
    {"date":"2026-04-23","totalTokens":     0,"totalCost":0.00},
    {"date":"2026-04-24","totalTokens":     0,"totalCost":0.00},
    {"date":"2026-04-25","totalTokens":2300000,"totalCost":9.78},
    {"date":"2026-04-26","totalTokens":3100000,"totalCost":13.91}
  ],
  "totals": { "totalTokens": 5400000, "totalCost": 23.69 }
}
```

`mac/fixtures/sample/projects/demo/abc123.jsonl` (mocks ~/.claude/projects/...; the earliest `type:"user"` timestamp here is what `weekly.started_at` should derive to):

```jsonl
{"timestamp":"2026-04-25T10:38:33.947Z","type":"user","sessionId":"abc123","message":{"content":"hello"}}
{"timestamp":"2026-04-25T10:38:34.012Z","type":"assistant","sessionId":"abc123"}
{"timestamp":"2026-04-26T09:00:00.000Z","type":"user","sessionId":"abc123","message":{"content":"again"}}
{"timestamp":"2026-04-26T09:00:01.000Z","type":"assistant","sessionId":"abc123"}
```

The fixture pinpoints `weekly.started_at = 2026-04-25T10:38:33Z = 1777718313` and therefore `weekly.resets_at = 1778323113` (= +7 days).

- [ ] **Step 2: Write the failing bats tests**

`mac/test/aggregate.bats`:

```bash
#!/usr/bin/env bats

setup() {
  ROOT="$(cd "$BATS_TEST_DIRNAME/.." && pwd)"
  PUSH="$ROOT/push-usage.sh"
  FIX="$ROOT/fixtures/sample"
  # Pin "now" so tests are deterministic. "Now" = 2026-04-26T15:30:00Z = 1777735800
  NOW=1777735800
}

run_push() {
  "$PUSH" --emit-only \
          --now "$NOW" \
          --ccusage-fixture-dir "$FIX" \
          --claude-dir "$FIX/projects"
}

@test "block_5h is sourced from ccusage blocks --active" {
  run run_push
  [ "$status" -eq 0 ]
  [ "$(jq -r '.block_5h.used_tokens' <<<"$output")" = "1017862" ]
  [ "$(jq -r '.block_5h.cost_usd'    <<<"$output")" = "1.81" ]
  [ "$(jq -r '.block_5h.messages'    <<<"$output")" = "11" ]
  [ "$(jq -r '.block_5h.burn_rate_tpm' <<<"$output")" = "82684" ]
  [ "$(jq -r '.block_5h.started_at'  <<<"$output")" = "1777734000" ]   # 2026-04-26T15:00Z
  [ "$(jq -r '.block_5h.resets_at'   <<<"$output")" = "1777752000" ]   # +5h
}

@test "weekly.started_at derives from earliest user event in trailing 168h" {
  run run_push
  [ "$status" -eq 0 ]
  [ "$(jq -r '.weekly.started_at' <<<"$output")" = "1777718313" ]
  [ "$(jq -r '.weekly.resets_at'  <<<"$output")" = "1778323113" ]      # +7d
}

@test "weekly.used_tokens sums daily entries within the window" {
  run run_push
  # Days 04-25 + 04-26 fall inside the weekly window (started 04-25T10:38)
  [ "$(jq -r '.weekly.used_tokens' <<<"$output")" = "5400000" ]
  [ "$(jq -r '.weekly.cost_usd'    <<<"$output")" = "23.69" ]
}

@test "today fields populate from current local date in daily.json" {
  run run_push
  [ "$(jq -r '.today.tokens'   <<<"$output")" = "3100000" ]
  [ "$(jq -r '.today.cost_usd' <<<"$output")" = "13.91" ]
}

@test "no quota fields leak into output (no percent, no limit_tokens)" {
  run run_push
  [ "$(jq -e '.block_5h | has("percent")' <<<"$output")" = "false" ]
  [ "$(jq -e '.block_5h | has("limit_tokens")' <<<"$output")" = "false" ]
  [ "$(jq -e '.weekly   | has("percent")' <<<"$output")" = "false" ]
  [ "$(jq -e '.weekly   | has("limit_tokens")' <<<"$output")" = "false" ]
}

@test "plan and ts present at root" {
  run run_push
  [ "$(jq -r '.plan' <<<"$output")" = "Max 5x" ]
  [ "$(jq -r '.ts'   <<<"$output")" = "1777735800" ]
}
```

- [ ] **Step 3: Run tests; expect them to fail**

Run: `bats mac/test/aggregate.bats`
Expected: 6 failures, all citing missing `push-usage.sh` or empty output.

- [ ] **Step 4: Implement `push-usage.sh`**

The script supports both real-mode (no flags) and test-mode (`--ccusage-fixture-dir`, `--claude-dir`, `--now`). Flags exist solely so bats can pin the data sources and clock. Required external tools: `jq`, `python3` (for ISO-8601 → epoch on macOS, since BSD `date` is awkward), `find`, `npx` (real mode only).

Skeleton:

```bash
#!/usr/bin/env bash
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

iso_to_epoch() { python3 -c 'import sys,datetime;print(int(datetime.datetime.fromisoformat(sys.argv[1].replace("Z","+00:00")).timestamp()))' "$1"; }

ccusage_blocks_json() {
  if [ -n "$CCUSAGE_FIXTURE_DIR" ]; then cat "$CCUSAGE_FIXTURE_DIR/blocks-active.json"
  else                                   npx ccusage@latest blocks --active --json 2>/dev/null
  fi
}

ccusage_daily_json() {
  if [ -n "$CCUSAGE_FIXTURE_DIR" ]; then cat "$CCUSAGE_FIXTURE_DIR/daily.json"
  else                                   npx ccusage@latest daily --json 2>/dev/null
  fi
}

# Earliest type:"user" timestamp in trailing 168h, scanning .jsonl recursively.
weekly_started_at() {
  local since=$(( NOW - 7*86400 ))
  local min=""
  while IFS= read -r ts; do
    local ep; ep=$(iso_to_epoch "$ts" 2>/dev/null || echo "")
    [ -z "$ep" ] && continue
    [ "$ep" -lt "$since" ] && continue
    [ -z "$min" ] && { min=$ep; continue; }
    [ "$ep" -lt "$min" ] && min=$ep
  done < <(find "$CLAUDE_DIR" -type f -name '*.jsonl' 2>/dev/null \
            | xargs -I {} jq -r 'select(.type=="user") | .timestamp' {} 2>/dev/null)
  echo "${min:-$since}"
}

build_payload() {
  local now="$1"
  local blocks daily wstart
  blocks="$(ccusage_blocks_json)"
  daily="$(ccusage_daily_json)"
  wstart="$(weekly_started_at)"

  jq -nc \
    --argjson now    "$now" \
    --argjson wstart "$wstart" \
    --arg     plan   "$PLAN_NAME" \
    --argjson blocks "$blocks" \
    --argjson daily  "$daily" \
    --arg     today_date "$(date -r "$now" +%Y-%m-%d)" \
    '
      ($blocks.blocks[0]) as $b |
      ($daily.daily // []) as $d |
      ($d | map(select(.date >= ($wstart | strftime("%Y-%m-%d"))))) as $win |
      ($d | map(select(.date == $today_date)) | first) as $today |
      {
        ts:   $now,
        plan: $plan,
        block_5h: ($b // {}) | {
          used_tokens:    (.totalTokens // 0),
          started_at:     (.startTime // null) | (if . then (
            (sub("Z$"; "+00:00")) | fromdateiso8601
          ) else 0 end),
          resets_at:      (.endTime  // null) | (if . then (
            (sub("Z$"; "+00:00")) | fromdateiso8601
          ) else 0 end),
          cost_usd:       (.costUSD // 0),
          messages:       (.entries // 0),
          burn_rate_tpm:  ((.burnRate.tokensPerMinute // 0) | floor)
        },
        weekly: {
          used_tokens: ($win | map(.totalTokens) | add // 0),
          started_at:  $wstart,
          resets_at:   ($wstart + 7*86400),
          cost_usd:    ($win | map(.totalCost)   | add // 0),
          messages:    ($win | length)
        },
        today: {
          tokens:   ($today.totalTokens // 0),
          messages: 0,
          cost_usd: ($today.totalCost  // 0)
        }
      }
    '
}

payload="$(build_payload "$NOW")"

if [ "$EMIT_ONLY" -eq 1 ]; then
  echo "$payload"
  exit 0
fi

# shellcheck disable=SC1091
. "$(dirname "$0")/secrets.env"
echo "$payload" | curl -fsS --max-time 10 \
  -X POST -H 'Content-Type: application/json' \
  --data @- "http://${HOST}/data"
```

> **Note on `today.messages`:** ccusage `daily` doesn't expose per-day message
> counts. v1 emits `0`; v2 may compute by counting `type:"user"` events in the
> jsonl scan. The device renders `—` when missing (or simply omits).

- [ ] **Step 5: Make script executable**

Run: `chmod +x mac/push-usage.sh`

- [ ] **Step 6: Run tests; expect them to pass**

Run: `bats mac/test/aggregate.bats`
Expected: `6 tests, 0 failures`.

- [ ] **Step 7: Commit**

```bash
git add mac/push-usage.sh mac/fixtures/sample/ mac/test/aggregate.bats
git commit -m "mac: push-usage.sh aggregation (5h block + weekly + today, no quota)"
git push origin main
```

### Task 6: `push-usage.sh` — real ccusage execution path

**Files:**
- Modify: `mac/push-usage.sh`
- Create: `mac/test/integration.bats`

- [ ] **Step 1: Write an integration test that calls ccusage for real**

Create `mac/test/integration.bats`. This test is gated — it skips if ccusage isn't available or the user has no Claude logs:

```bash
#!/usr/bin/env bats

setup() {
  PUSH="$BATS_TEST_DIRNAME/../push-usage.sh"
}

@test "real ccusage path emits valid wire JSON" {
  if ! command -v npx >/dev/null; then skip "no npx"; fi
  if [ ! -d "$HOME/.claude/projects" ]; then skip "no Claude logs"; fi
  run "$PUSH" --emit-only
  [ "$status" -eq 0 ]
  echo "$output" | jq -e '
    has("ts") and has("plan")
      and (.block_5h | has("used_tokens") and has("started_at") and has("resets_at"))
      and (.weekly   | has("used_tokens") and has("started_at") and has("resets_at"))
  '
}
```

- [ ] **Step 2: Run; either passes or skips**

Run: `bats mac/test/integration.bats`
Expected: passes if the user has Claude usage history; otherwise skips cleanly.

- [ ] **Step 3: If the real ccusage shape differs from the fixture, adjust `aggregate()` and re-run both bats files until both pass**

Run: `bats mac/test/`
Expected: all tests pass.

- [ ] **Step 4: Commit**

```bash
git add mac/test/integration.bats mac/push-usage.sh
git commit -m "mac: real ccusage integration path verified"
```

### Task 7: launchd plist + install instructions

**Files:**
- Create: `mac/com.rock.ai-usage-push.plist`
- Modify: `README.md` (no change needed if Task 0 already covers this)

- [ ] **Step 1: Write the launchd plist**

Create `mac/com.rock.ai-usage-push.plist` (replace `/Users/rock` if different — the plist will be customized per machine via the install step):

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key>
  <string>com.rock.ai-usage-push</string>
  <key>ProgramArguments</key>
  <array>
    <string>/Users/rock/code/esp32-ai-usage-display/mac/push-usage.sh</string>
  </array>
  <key>RunAtLoad</key>
  <true/>
  <key>StartInterval</key>
  <integer>60</integer>
  <key>StandardOutPath</key>
  <string>/Users/rock/Library/Logs/ai-usage-push.log</string>
  <key>StandardErrorPath</key>
  <string>/Users/rock/Library/Logs/ai-usage-push.log</string>
  <key>EnvironmentVariables</key>
  <dict>
    <key>PATH</key>
    <string>/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin</string>
  </dict>
</dict>
</plist>
```

- [ ] **Step 2: Validate plist syntax**

Run: `plutil mac/com.rock.ai-usage-push.plist`
Expected: `mac/com.rock.ai-usage-push.plist: OK`.

- [ ] **Step 3: Commit**

```bash
git add mac/com.rock.ai-usage-push.plist
git commit -m "mac: launchd plist for 60s push interval"
```

### Task 8: `test-push.sh` helper for firmware fixture testing

**Files:**
- Create: `mac/test-push.sh`
- Create: `mac/fixtures/edge-cases/` with a few JSON fixtures

- [ ] **Step 1: Create edge-case fixtures**

```bash
mkdir -p mac/fixtures/edge-cases
```

Each fixture is a **wire-schema** payload (what `push-usage.sh` would have emitted) — the helper POSTs them straight to the device for visual verification.

`mac/fixtures/edge-cases/idle.json` — empty 5h block (idle ≥ 5 h):

```json
{"ts":1777735800,"plan":"Max 5x","block_5h":{"used_tokens":0,"started_at":1777734000,"resets_at":1777752000,"cost_usd":0,"messages":0,"burn_rate_tpm":0},"weekly":{"used_tokens":0,"started_at":1777131000,"resets_at":1777735800,"cost_usd":0,"messages":0},"today":{"tokens":0,"messages":0,"cost_usd":0}}
```

`mac/fixtures/edge-cases/full.json` — large numbers exercise the K/M formatter:

```json
{"ts":1777735800,"plan":"Max 5x","block_5h":{"used_tokens":24500000,"started_at":1777734000,"resets_at":1777752000,"cost_usd":89.42,"messages":214,"burn_rate_tpm":480000},"weekly":{"used_tokens":312000000,"started_at":1777200000,"resets_at":1777804800,"cost_usd":1187.10,"messages":1430},"today":{"tokens":52000000,"messages":311,"cost_usd":201.50}}
```

`mac/fixtures/edge-cases/missing-today.json` — the optional `today` block absent:

```json
{"ts":1777735800,"plan":"Max 5x","block_5h":{"used_tokens":1017862,"started_at":1777734000,"resets_at":1777752000,"cost_usd":1.81,"messages":11,"burn_rate_tpm":82684},"weekly":{"used_tokens":5400000,"started_at":1777718313,"resets_at":1778323113,"cost_usd":23.69,"messages":2}}
```

`mac/fixtures/edge-cases/no-burn.json` — burn rate omitted (e.g. immediately after a fresh block boundary):

```json
{"ts":1777735800,"plan":"Max 5x","block_5h":{"used_tokens":42000,"started_at":1777735200,"resets_at":1777753200,"cost_usd":0.08,"messages":1},"weekly":{"used_tokens":42000,"started_at":1777735200,"resets_at":1778340000,"cost_usd":0.08,"messages":1},"today":{"tokens":42000,"messages":1,"cost_usd":0.08}}
```

- [ ] **Step 2: Write the helper**

Create `mac/test-push.sh`:

```bash
#!/usr/bin/env bash
# Usage: ./test-push.sh fixtures/edge-cases/zero.json
# Posts a fixture to the device for manual visual verification.
set -euo pipefail
[ $# -eq 1 ] || { echo "usage: $0 <fixture.json>" >&2; exit 64; }
. "$(dirname "$0")/secrets.env"
curl -fsS -X POST -H 'Content-Type: application/json' \
  --data @"$1" "http://${HOST}/data"
echo
```

- [ ] **Step 3: Make executable and commit**

```bash
chmod +x mac/test-push.sh
git add mac/test-push.sh mac/fixtures/edge-cases/
git commit -m "mac: test-push.sh helper + edge-case fixtures"
```

---

## Phase C — ESP32 firmware

### Task 9: PlatformIO scaffold + serial-only smoke

**Files:**
- Create: `firmware/platformio.ini`
- Create: `firmware/src/main.cpp`
- Create: `firmware/src/secrets.h.example`

- [ ] **Step 1: platformio.ini**

```ini
; firmware/platformio.ini
[env:rlcd42]
platform = espressif32@^6.7.0
board = esp32-s3-devkitc-1
framework = arduino
monitor_speed = 115200
upload_speed = 921600
board_build.arduino.memory_type = qio_opi
board_build.partitions = default_16MB.csv
board_upload.flash_size = 16MB
build_flags =
  -DBOARD_HAS_PSRAM
  -DARDUINO_USB_MODE=1
  -DARDUINO_USB_CDC_ON_BOOT=1
lib_deps =
  lovyan03/LovyanGFX@^1.1.16
  bblanchon/ArduinoJson@^7.1.0
```

- [ ] **Step 2: secrets.h.example**

```cpp
// firmware/src/secrets.h.example
#pragma once
// Copy to firmware/src/secrets.h (gitignored) and edit.
#define WIFI_SSID "your-ssid"
#define WIFI_PWD  "your-password"
```

- [ ] **Step 3: Minimal main.cpp**

```cpp
// firmware/src/main.cpp
#include <Arduino.h>

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("[boot] esp32-ai-usage-display v0");
}

void loop() {
  Serial.println("[heartbeat]");
  delay(5000);
}
```

- [ ] **Step 4: Build and flash**

```bash
cd firmware
cp src/secrets.h.example src/secrets.h   # placeholder, not used yet
pio run -t upload
pio device monitor
```

Expected: `[boot] esp32-ai-usage-display v0` followed by repeating `[heartbeat]`.

- [ ] **Step 5: Commit**

```bash
git add firmware/platformio.ini firmware/src/main.cpp firmware/src/secrets.h.example
git commit -m "firmware: PlatformIO scaffold with serial heartbeat"
```

### Task 10: Wi-Fi STA connect

**Files:**
- Modify: `firmware/src/main.cpp`

- [ ] **Step 1: Edit `firmware/src/secrets.h`** with real SSID + password (do NOT commit this file)

- [ ] **Step 2: Replace main.cpp with Wi-Fi connect logic**

```cpp
// firmware/src/main.cpp
#include <Arduino.h>
#include <WiFi.h>
#include "secrets.h"

static void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PWD);
  Serial.printf("[wifi] connecting to %s ", WIFI_SSID);
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
  }
  Serial.printf("\n[wifi] OK ip=%s\n", WiFi.localIP().toString().c_str());
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("[boot] esp32-ai-usage-display v0");
  connectWifi();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[wifi] dropped, reconnecting");
    connectWifi();
  }
  delay(5000);
}
```

- [ ] **Step 3: Flash, observe**

Run: `pio run -t upload && pio device monitor`
Expected: `[wifi] OK ip=...` printed within ~10 s of boot.

- [ ] **Step 4: Commit**

```bash
git add firmware/src/main.cpp
git commit -m "firmware: Wi-Fi STA connect with reconnect loop"
```

### Task 11: mDNS broadcast as `ai-usage-display.local`

**Files:**
- Modify: `firmware/src/main.cpp`

- [ ] **Step 1: Add ESPmDNS to setup**

Insert after `connectWifi();`:

```cpp
#include <ESPmDNS.h>

static void startMdns() {
  if (!MDNS.begin("ai-usage-display")) {
    Serial.println("[mdns] FAILED");
    return;
  }
  MDNS.addService("http", "tcp", 80);
  Serial.println("[mdns] ai-usage-display.local advertised");
}
```

Add `startMdns();` immediately after `connectWifi();` in `setup()`.

- [ ] **Step 2: Flash, observe**

Run: `pio run -t upload && pio device monitor`
Expected: `[mdns] ai-usage-display.local advertised`.

- [ ] **Step 3: From the Mac, verify resolution**

Run: `ping -c 2 ai-usage-display.local`
Expected: replies from the device's IP.

- [ ] **Step 4: Commit**

```bash
git add firmware/src/main.cpp
git commit -m "firmware: mDNS broadcast as ai-usage-display.local"
```

### Task 12: HTTP server + `POST /data` with native unit test for parser

**Files:**
- Create: `firmware/src/state.h`
- Create: `firmware/src/api.cpp`
- Create: `firmware/src/api.h`
- Create: `firmware/test/test_api/test_api.cpp`
- Modify: `firmware/src/main.cpp`
- Modify: `firmware/platformio.ini` (add native test env)

- [ ] **Step 1: Write `state.h`**

```cpp
// firmware/src/state.h
#pragma once
#include <stdint.h>

struct UsageData {
  uint32_t ts = 0;
  char     plan[16] = {0};

  // 5h block (required fields → started_at, resets_at, used_tokens)
  uint64_t tok_5h        = 0;
  uint32_t started_5h    = 0;
  uint32_t reset_5h      = 0;
  double   cost_5h_usd   = 0.0;
  uint32_t msgs_5h       = 0;
  uint32_t burn_tpm      = 0;
  bool     burn_present  = false;
  bool     cost_5h_present = false;
  bool     msgs_5h_present = false;

  // weekly (required fields → started_at, resets_at, used_tokens)
  uint64_t tok_weekly       = 0;
  uint32_t started_weekly   = 0;
  uint32_t reset_weekly     = 0;
  double   cost_weekly_usd  = 0.0;
  uint32_t msgs_weekly      = 0;
  bool     cost_weekly_present = false;
  bool     msgs_weekly_present = false;

  // today (whole object optional)
  uint64_t tok_today       = 0;
  uint32_t msgs_today      = 0;
  double   cost_today_usd  = 0.0;
  bool     today_present   = false;

  bool     valid = false;
};
```

- [ ] **Step 2: Write `api.h`**

```cpp
// firmware/src/api.h
#pragma once
#include "state.h"

// Returns true and populates `out` on valid payload; false on malformed.
bool parseUsageJson(const char* body, UsageData& out);
```

- [ ] **Step 3: Write `api.cpp`**

```cpp
// firmware/src/api.cpp
#include "api.h"
#include <ArduinoJson.h>
#include <string.h>

bool parseUsageJson(const char* body, UsageData& out) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  if (!doc["ts"].is<uint32_t>()) return false;

  // Required block_5h fields
  if (!doc["block_5h"]["used_tokens"].is<uint64_t>()) return false;
  if (!doc["block_5h"]["started_at"].is<uint32_t>())  return false;
  if (!doc["block_5h"]["resets_at"].is<uint32_t>())   return false;
  // Required weekly fields
  if (!doc["weekly"]["used_tokens"].is<uint64_t>()) return false;
  if (!doc["weekly"]["started_at"].is<uint32_t>())  return false;
  if (!doc["weekly"]["resets_at"].is<uint32_t>())   return false;

  out.ts = doc["ts"];
  strlcpy(out.plan, doc["plan"] | "", sizeof(out.plan));

  // 5h block
  auto b = doc["block_5h"];
  out.tok_5h     = b["used_tokens"];
  out.started_5h = b["started_at"];
  out.reset_5h   = b["resets_at"];
  if (b["cost_usd"].is<double>())      { out.cost_5h_usd   = b["cost_usd"];      out.cost_5h_present = true; }
  if (b["messages"].is<uint32_t>())    { out.msgs_5h       = b["messages"];      out.msgs_5h_present = true; }
  if (b["burn_rate_tpm"].is<uint32_t>()){ out.burn_tpm      = b["burn_rate_tpm"]; out.burn_present    = true; }

  // weekly
  auto w = doc["weekly"];
  out.tok_weekly     = w["used_tokens"];
  out.started_weekly = w["started_at"];
  out.reset_weekly   = w["resets_at"];
  if (w["cost_usd"].is<double>())   { out.cost_weekly_usd  = w["cost_usd"]; out.cost_weekly_present = true; }
  if (w["messages"].is<uint32_t>()) { out.msgs_weekly      = w["messages"]; out.msgs_weekly_present = true; }

  // today (optional whole object)
  if (!doc["today"].isNull()) {
    auto t = doc["today"];
    out.tok_today      = t["tokens"]   | 0;
    out.msgs_today     = t["messages"] | 0;
    out.cost_today_usd = t["cost_usd"] | 0.0;
    out.today_present  = true;
  }

  out.valid = true;
  return true;
}
```

- [ ] **Step 4: Add native test environment to platformio.ini**

Append:

```ini
[env:native]
platform = native
test_framework = unity
build_flags = -std=gnu++17 -I src
lib_deps =
  bblanchon/ArduinoJson@^7.1.0
```

- [ ] **Step 5: Write the failing host-side test**

Create `firmware/test/test_api/test_api.cpp`:

```cpp
#include <unity.h>
#include "api.h"

void test_valid_payload() {
  const char* body = R"({
    "ts": 1777735800, "plan": "Max 5x",
    "block_5h": {"used_tokens": 1017862, "started_at": 1777734000, "resets_at": 1777752000,
                 "cost_usd": 1.81, "messages": 11, "burn_rate_tpm": 82684},
    "weekly":   {"used_tokens": 5400000, "started_at": 1777718313, "resets_at": 1778323113,
                 "cost_usd": 23.69, "messages": 47},
    "today":    {"tokens": 2100000, "messages": 14, "cost_usd": 5.62}
  })";
  UsageData u;
  TEST_ASSERT_TRUE(parseUsageJson(body, u));
  TEST_ASSERT_EQUAL_UINT32(1777735800, u.ts);
  TEST_ASSERT_EQUAL_STRING("Max 5x", u.plan);
  TEST_ASSERT_EQUAL_UINT64(1017862, u.tok_5h);
  TEST_ASSERT_EQUAL_UINT32(1777734000, u.started_5h);
  TEST_ASSERT_EQUAL_UINT32(1777752000, u.reset_5h);
  TEST_ASSERT_EQUAL_UINT32(82684, u.burn_tpm);
  TEST_ASSERT_TRUE(u.burn_present);
  TEST_ASSERT_EQUAL_UINT64(5400000, u.tok_weekly);
  TEST_ASSERT_EQUAL_UINT32(1777718313, u.started_weekly);
  TEST_ASSERT_EQUAL_UINT32(1778323113, u.reset_weekly);
  TEST_ASSERT_TRUE(u.today_present);
  TEST_ASSERT_EQUAL_UINT64(2100000, u.tok_today);
}

void test_missing_today_is_ok() {
  const char* body = R"({
    "ts": 1, "plan": "x",
    "block_5h":{"used_tokens":0,"started_at":0,"resets_at":0},
    "weekly":  {"used_tokens":0,"started_at":0,"resets_at":0}
  })";
  UsageData u;
  TEST_ASSERT_TRUE(parseUsageJson(body, u));
  TEST_ASSERT_FALSE(u.today_present);
  TEST_ASSERT_FALSE(u.burn_present);
  TEST_ASSERT_FALSE(u.cost_5h_present);
}

void test_missing_burn_is_ok() {
  const char* body = R"({
    "ts":1,"plan":"x",
    "block_5h":{"used_tokens":42000,"started_at":1,"resets_at":2,"cost_usd":0.08,"messages":1},
    "weekly":  {"used_tokens":42000,"started_at":1,"resets_at":2}
  })";
  UsageData u;
  TEST_ASSERT_TRUE(parseUsageJson(body, u));
  TEST_ASSERT_FALSE(u.burn_present);
  TEST_ASSERT_TRUE(u.cost_5h_present);
  TEST_ASSERT_TRUE(u.msgs_5h_present);
}

void test_malformed_rejects() {
  UsageData u;
  TEST_ASSERT_FALSE(parseUsageJson("not json", u));
  TEST_ASSERT_FALSE(parseUsageJson("{}", u));
  // Missing required block_5h.started_at:
  TEST_ASSERT_FALSE(parseUsageJson(
    R"({"ts":1,"plan":"x","block_5h":{"used_tokens":0,"resets_at":0},"weekly":{"used_tokens":0,"started_at":0,"resets_at":0}})",
    u));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_valid_payload);
  RUN_TEST(test_missing_today_is_ok);
  RUN_TEST(test_missing_burn_is_ok);
  RUN_TEST(test_malformed_rejects);
  return UNITY_END();
}
```

- [ ] **Step 6: Run native tests**

Run: `pio test -e native`
Expected: 4 tests pass.

- [ ] **Step 7: Wire the HTTP server**

Replace `firmware/src/main.cpp` (preserving Wi-Fi + mDNS already added):

```cpp
#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "secrets.h"
#include "state.h"
#include "api.h"

static WebServer server(80);
static UsageData g_state;
static SemaphoreHandle_t g_mutex;
static volatile bool g_dirty = true;
static volatile uint32_t g_last_post_ms = 0;

static void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PWD);
  Serial.printf("[wifi] connecting to %s ", WIFI_SSID);
  while (WiFi.status() != WL_CONNECTED) { Serial.print("."); delay(500); }
  Serial.printf("\n[wifi] OK ip=%s\n", WiFi.localIP().toString().c_str());
}

static void startMdns() {
  if (!MDNS.begin("ai-usage-display")) { Serial.println("[mdns] FAILED"); return; }
  MDNS.addService("http", "tcp", 80);
  Serial.println("[mdns] ai-usage-display.local advertised");
}

static void handleData() {
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "no body"); return; }
  UsageData parsed;
  if (!parseUsageJson(server.arg("plain").c_str(), parsed)) {
    server.send(400, "text/plain", "bad json");
    return;
  }
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  g_state = parsed;
  g_dirty = true;
  g_last_post_ms = millis();
  xSemaphoreGive(g_mutex);
  Serial.printf("[api] state updated tok_5h=%llu tok_w=%llu\n",
                (unsigned long long)parsed.tok_5h, (unsigned long long)parsed.tok_weekly);
  server.send(200, "text/plain", "ok");
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("[boot] esp32-ai-usage-display v0");
  g_mutex = xSemaphoreCreateMutex();
  connectWifi();
  startMdns();
  server.on("/data", HTTP_POST, handleData);
  server.begin();
  Serial.println("[http] server on :80");
}

void loop() {
  server.handleClient();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[wifi] dropped, reconnecting");
    connectWifi();
    startMdns();
  }
  delay(10);
}
```

- [ ] **Step 8: Flash, send a fixture, observe state log**

Run on device: `pio run -t upload && pio device monitor`
Run on Mac: `cd mac && ./test-push.sh fixtures/edge-cases/idle.json`
Expected (device serial): `[api] state updated tok_5h=0 tok_w=0` and `200 OK` returned to curl.

- [ ] **Step 9: Commit**

```bash
git add firmware/src/state.h firmware/src/api.h firmware/src/api.cpp \
        firmware/test/test_api/test_api.cpp firmware/src/main.cpp \
        firmware/platformio.ini
git commit -m "firmware: HTTP /data endpoint + native-tested JSON parser"
```

### Task 13: LCD init via vendored Waveshare BSP + 1-bit canvas; draw "OK"

**Files:**
- Create: `firmware/src/display.h`
- Create: `firmware/src/display.cpp`
- Vendor:  `firmware/src/display_bsp.h`, `firmware/src/display_bsp.cpp`
- Modify:  `firmware/src/main.cpp`
- Modify:  `firmware/platformio.ini` (no LovyanGFX panel class needed; keep lib for `LGFX_Sprite`)

Background: the Waveshare ESP32-S3-RLCD-4.2 ships a Sitronix ST7305 mono reflective LCD with a non-linear 4×2 tile framebuffer. There is **no upstream LovyanGFX panel driver**. Strategy (per `docs/superpowers/notes/2026-04-26-lcd-driver.md` Option A): vendor Waveshare's `display_bsp.{h,cpp}` from their official demo for chip init + tile-packing blit; use `LGFX_Sprite` (color depth 1) as the linear 1-bpp canvas the renderer draws into; on commit, walk the canvas buffer with the BSP's tile-packer and push to the panel.

- [ ] **Step 1: Vendor Waveshare's `display_bsp.{h,cpp}`**

From the demo zip referenced in `docs/superpowers/notes/2026-04-26-lcd-driver.md`, copy the two files into `firmware/src/`. Before copying, run `head -30` on each to confirm there's no incompatible license header — if there is, escalate. Strip any unrelated includes (e.g. board-specific touch / SD bringup) so the file only initializes the LCD.

The two BSP entry points we depend on:

```c
// display_bsp.h (vendored verbatim)
void  display_bsp_init(void);                            // chip reset + init seq + sleep-out (~250 ms)
void  display_bsp_blit_1bpp(const uint8_t* canvas);      // canvas is W*H/8 packed bytes,
                                                         // row-major, 0=white 1=black, native 300x400
```

Adjust the function names to whatever the vendor file exposes — the `display.cpp` wrapper below decouples renderers from these names.

- [ ] **Step 2: `display.h`**

```cpp
// firmware/src/display.h
#pragma once
#include <LovyanGFX.hpp>

// Initializes the LCD and the 1-bit canvas. Must be called from setup().
void displayInit();

// Renderer draws into this 400x300 1-bpp sprite. (Software rotation: the
// vendor BSP expects 300x400 native; our canvas is 400x300 landscape and
// displayCommit() rotates while packing.)
LGFX_Sprite& displayCanvas();

// Pushes the canvas to the panel. Call once per dirty render, not per draw op.
void displayCommit();
```

- [ ] **Step 3: `display.cpp` — wraps BSP, owns the canvas, does landscape→native rotation**

```cpp
// firmware/src/display.cpp
#include "display.h"
extern "C" {
  #include "display_bsp.h"
}

static LGFX_Sprite g_canvas;
static uint8_t     g_native_fb[300 * 400 / 8];   // 15000 bytes; native portrait packed 1-bpp

void displayInit() {
  display_bsp_init();
  g_canvas.setColorDepth(1);
  // Palette: index 0 = white (background, off-pixel), index 1 = black (ink).
  g_canvas.setPaletteColor(0, 0xFFFFFF);
  g_canvas.setPaletteColor(1, 0x000000);
  g_canvas.createSprite(400, 300);
  g_canvas.fillScreen(0);
  displayCommit();
}

LGFX_Sprite& displayCanvas() { return g_canvas; }

void displayCommit() {
  // Walk the landscape canvas (400 wide × 300 tall) and pack pixels into
  // the native portrait framebuffer (300 wide × 400 tall) row-major 1-bpp.
  // Each native pixel (nx, ny) corresponds to canvas (cx=ny, cy=300-1-nx).
  // (This matches a 90° clockwise rotation; flip if the panel reads upside-down.)
  for (int ny = 0; ny < 400; ++ny) {
    for (int nx = 0; nx < 300; ++nx) {
      int cx = ny;
      int cy = 299 - nx;
      bool ink = g_canvas.readPixelValue(cx, cy) != 0;
      int bit_index = ny * 300 + nx;
      uint8_t mask = 1 << (7 - (bit_index & 7));
      uint8_t& byte = g_native_fb[bit_index >> 3];
      if (ink) byte |=  mask;
      else     byte &= ~mask;
    }
  }
  display_bsp_blit_1bpp(g_native_fb);
}
```

> **Caveat:** the rotation choice (which corner is "up") and any horizontal
> mirroring must be confirmed empirically on first flash — flip `cx`/`cy` mappings
> if "OK" appears upside-down or mirrored. The exact form depends on the panel's
> default scan direction, which Task 2's notes record but the renderer doesn't
> need to reason about.

- [ ] **Step 4: Draw "OK" from `displayInit()` after canvas creation**

Insert before the trailing `displayCommit()` in `displayInit()`:

```cpp
g_canvas.setTextColor(1);            // ink
g_canvas.setTextSize(3);
g_canvas.setCursor(20, 20);
g_canvas.print("OK");
```

- [ ] **Step 5: Call `displayInit()` from `setup()`**

In `firmware/src/main.cpp`, add `#include "display.h"` and call `displayInit();` in `setup()` immediately after `Serial.begin/println` and before `connectWifi()`.

- [ ] **Step 6: Flash and observe**

Run: `pio run -t upload && pio device monitor`
Expected: panel shows "OK" in the upper-left of the landscape view. If the
text is rotated/mirrored, swap the rotation mapping in `displayCommit()` and
reflash.

- [ ] **Step 7: Commit**

```bash
git add firmware/src/display.h firmware/src/display.cpp \
        firmware/src/display_bsp.h firmware/src/display_bsp.cpp \
        firmware/src/main.cpp firmware/platformio.ini
git commit -m "firmware: vendored ST7305 BSP + 1-bpp canvas; draws OK"
git push origin main
```

### Task 14: Render — header strip

**Files:**
- Create: `firmware/src/render.h`
- Create: `firmware/src/render.cpp`
- Modify: `firmware/src/main.cpp`

- [ ] **Step 1: Write render.h**

```cpp
// firmware/src/render.h
#pragma once
#include <stdint.h>
#include "state.h"
void renderInit();
// ms_since_post: millis() - last_post_millis on caller side
void renderTick(const UsageData& s, bool stale, bool wifi_ok, uint32_t ms_since_post);
```

- [ ] **Step 2: Header drawing in render.cpp**

```cpp
// firmware/src/render.cpp
#include "render.h"
#include "display.h"

static LGFX_Sprite* d = nullptr;
static constexpr uint8_t INK = 1;
static constexpr uint8_t BG  = 0;

void renderInit() { d = &displayCanvas(); }

static void drawHeader(const UsageData& s, bool wifi_ok) {
  d->fillRect(0, 0, 400, 24, BG);
  d->setTextColor(INK);
  d->setTextSize(2);
  d->setCursor(8, 4);
  d->printf("CLAUDE CODE  %s", s.plan[0] ? s.plan : "");
  if (!wifi_ok) {
    d->setCursor(380, 4);
    d->print("!");          // simple Wi-Fi-down marker; refined in Task 18
  }
  d->drawLine(0, 24, 400, 24, INK);
}

void renderTick(const UsageData& s, bool stale, bool wifi_ok, uint32_t ms_since_post) {
  (void)stale; (void)ms_since_post;          // used in later tasks
  d->fillScreen(BG);
  drawHeader(s, wifi_ok);
  displayCommit();
}
```

- [ ] **Step 3: Wire into main loop**

In `main.cpp`, add `#include "render.h"`, call `renderInit();` after `displayInit();`, and in `loop()` add (every ~1 s):

```cpp
static uint32_t last_render = 0;
if (millis() - last_render >= 1000) {
  last_render = millis();
  uint32_t age = millis() - g_last_post_ms;
  bool wifi_ok = WiFi.status() == WL_CONNECTED;
  bool stale = age > 300000UL;
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  UsageData snap = g_state;
  xSemaphoreGive(g_mutex);
  renderTick(snap, stale, wifi_ok, age);
}
```

- [ ] **Step 4: Flash, post a fixture, observe header**

Expected: top strip shows `CLAUDE CODE  Max 5x` against a separator line.

- [ ] **Step 5: Commit**

```bash
git add firmware/src/render.h firmware/src/render.cpp firmware/src/main.cpp
git commit -m "firmware: render header strip"
```

### Task 15: Render — 5h and weekly sections (tokens-first, time-progress bar)

**Files:**
- Modify: `firmware/src/render.cpp`

The dominant figure on each row is **tokens used** (compact K/M format). The bar's fill represents **time progress through the window**: `(now − started_at) / (resets_at − started_at)`. The meta line below shows `$cost · N msg · burnK/min` (left) and `resets in NN` (right). See spec §7.

- [ ] **Step 1: Add formatting helpers**

```cpp
// near the top of render.cpp
static void formatTokens(uint64_t n, char* out, size_t sz) {
  if      (n >= 1000000ULL) snprintf(out, sz, "%.1fM", n / 1000000.0);
  else if (n >= 1000ULL)    snprintf(out, sz, "%uK", (unsigned)(n / 1000));
  else                      snprintf(out, sz, "%u",  (unsigned)n);
}

static int textWidth(LGFX_Sprite* d, const char* s, int size) {
  return d->textWidth(s) * size / d->textsize_x;   // approximate; refined empirically
}
```

- [ ] **Step 2: Add the `drawWindow` for tokens + time-progress bar**

```cpp
static void drawWindow(int y, const char* label, uint64_t used,
                       uint32_t started, uint32_t resets, uint32_t now,
                       const char* meta_left, const char* meta_right) {
  // Label (top-left, size 2)
  d->setTextSize(2);
  d->setTextColor(INK);
  d->setCursor(8, y);
  d->print(label);

  // Big tokens (top-right, size 4)
  char tok_str[16];
  formatTokens(used, tok_str, sizeof tok_str);
  d->setTextSize(4);
  int tw = textWidth(d, tok_str, 4);
  d->setCursor(392 - tw, y - 6);
  d->print(tok_str);

  // Time-progress bar: full width, fill = elapsed / total
  int barY = y + 28;
  d->drawRect(8, barY, 384, 16, INK);
  if (resets > started) {
    uint32_t total   = resets - started;
    uint32_t elapsed = (now > started) ? (now - started) : 0;
    if (elapsed > total) elapsed = total;
    int fill = (int)((uint64_t)elapsed * 380 / total);
    if (fill > 0) d->fillRect(10, barY + 2, fill, 12, INK);
  }

  // Meta line (size 1)
  d->setTextSize(1);
  d->setCursor(8, barY + 22);
  d->print(meta_left);
  if (meta_right[0]) {
    int rw = textWidth(d, meta_right, 1);
    d->setCursor(392 - rw, barY + 22);
    d->print(meta_right);
  }
}

// Compose the 5h-block meta strings from optional fields. fmtDuration is
// added in Task 16 — for now leave a stub so this compiles.
static void fmtDuration(uint32_t secs, char* out, size_t n);  // forward-decl

static void buildMeta5h(const UsageData& s,
                        char* left, size_t lsz, char* right, size_t rsz) {
  char m[16] = "", b[16] = "";
  if (s.msgs_5h_present) snprintf(m, sizeof m, " · %u msg", s.msgs_5h);
  if (s.burn_present)    snprintf(b, sizeof b, " · %uK/min", s.burn_tpm / 1000);
  if (s.cost_5h_present) snprintf(left, lsz, "$%.2f%s%s", s.cost_5h_usd, m, b);
  else                   left[0] = 0;
  if (s.reset_5h > s.ts) {
    char buf[16]; fmtDuration(s.reset_5h - s.ts, buf, sizeof buf);
    snprintf(right, rsz, "resets in %s", buf);
  } else right[0] = 0;
}

static void buildMetaWeekly(const UsageData& s,
                            char* left, size_t lsz, char* right, size_t rsz) {
  char m[16] = "";
  if (s.msgs_weekly_present) snprintf(m, sizeof m, " · %u msg", s.msgs_weekly);
  if (s.cost_weekly_present) snprintf(left, lsz, "$%.2f%s", s.cost_weekly_usd, m);
  else                       left[0] = 0;
  if (s.reset_weekly > s.ts) {
    char buf[16]; fmtDuration(s.reset_weekly - s.ts, buf, sizeof buf);
    snprintf(right, rsz, "resets in %s", buf);
  } else right[0] = 0;
}
```

> Note: `fmtDuration` is defined in Task 16. The forward declaration here keeps
> compilation green during Task 15 — until Task 16 lands the function body, the
> meta-right output for both rows will be a linker error if exercised. Easiest
> path is to do Task 15 and Task 16 back-to-back without flashing in between,
> or stub the function body in Task 15 with a `snprintf("%us", secs)` and
> replace it in Task 16.

- [ ] **Step 3: Wire into `renderTick`**

```cpp
void renderTick(const UsageData& s, bool stale, bool wifi_ok, uint32_t ms_since_post) {
  d->fillScreen(BG);
  drawHeader(s, wifi_ok);
  if (!s.valid) { displayCommit(); return; }   // Task 17 fills this in

  char l5[40], r5[24], lw[40], rw[24];
  buildMeta5h(s,    l5, sizeof l5, r5, sizeof r5);
  buildMetaWeekly(s, lw, sizeof lw, rw, sizeof rw);

  drawWindow( 36, "5H BLOCK", s.tok_5h,     s.started_5h,     s.reset_5h,     s.ts, l5, r5);
  drawWindow(140, "WEEKLY",   s.tok_weekly, s.started_weekly, s.reset_weekly, s.ts, lw, rw);

  (void)stale; (void)ms_since_post;          // used in Task 16's footer
  displayCommit();
}
```

- [ ] **Step 4: Flash + post `mac/fixtures/edge-cases/idle.json` then `full.json`, photograph each**

- `idle.json` → both rows show `0` tokens, bars empty, meta line `$0.00 · 0 msg`.
- `full.json` → tokens show as `24.5M` / `312.0M`; both bars near full (depending on `started/resets/now` choice in fixture); meta line shows large $ and message counts.

Adjust text positions if tokens like `312.0M` don't fit at size-4 within the 400 px width.

- [ ] **Step 5: Commit**

```bash
git add firmware/src/render.cpp
git commit -m "firmware: render tokens-first 5h block + weekly with time-progress bar"
git push origin main
```

### Task 16: Render — footer, STALE, and reset countdown formatting

**Files:**
- Modify: `firmware/src/render.cpp`

- [ ] **Step 1: Add reset duration formatter and use it in `drawWindow`**

Add this helper near the top of `render.cpp`:

```cpp
static void fmtDuration(uint32_t secs, char* out, size_t n) {
  uint32_t d = secs / 86400; secs %= 86400;
  uint32_t h = secs / 3600;  secs %= 3600;
  uint32_t m = secs / 60;
  if (d > 0)      snprintf(out, n, "%lud %luh", (unsigned long)d, (unsigned long)h);
  else if (h > 0) snprintf(out, n, "%luh %lum", (unsigned long)h, (unsigned long)m);
  else            snprintf(out, n, "%lum",      (unsigned long)m);
}
```

Replace the temporary `%lus` block at the bottom of `drawWindow` with:

```cpp
  if (resets_at > now) {
    char buf[16];
    fmtDuration(resets_at - now, buf, sizeof buf);
    d->setCursor(220, barY + 22);
    d->printf("resets in %s", buf);
  }
```

- [ ] **Step 2: Footer**

```cpp
static void drawFooter(const UsageData& s, bool stale, uint32_t now_ms_since_post) {
  int y = 270;
  d->drawLine(0, y - 4, 400, y - 4, INK);
  d->setTextSize(1);
  d->setTextColor(INK);
  d->setCursor(8, y);
  if (s.today_present) {
    char tok[16];
    formatTokens(s.tok_today, tok, sizeof tok);
    if (s.msgs_today > 0)
      d->printf("Today: %s tok · %u msg", tok, s.msgs_today);
    else
      d->printf("Today: %s tok", tok);
  }
  d->setCursor(270, y);
  if (stale) d->print("STALE  ");
  uint32_t age_min = now_ms_since_post / 60000UL;
  d->printf("upd %lum ago", (unsigned long)age_min);
}
```

- [ ] **Step 3: Hook into `renderTick`**

`renderTick`'s signature already takes `ms_since_post` from Task 14 — just pass it through. After the existing `drawWindow(140, ...)` call, add:

```cpp
  drawFooter(s, stale, ms_since_post);
```

The `(void)stale; (void)ms_since_post;` placeholder added in Task 14 should be deleted now that both arguments are real.

- [ ] **Step 4: Flash + post `missing-today.json`; verify footer omits `Today:` and just shows `upd Nm ago`. Wait > 5 min without posting, expect `STALE`.**

- [ ] **Step 5: Commit**

```bash
git add firmware/src/render.cpp firmware/src/render.h
git commit -m "firmware: footer with STALE detection and reset countdown"
```

### Task 17: First-sync waiting screen

**Files:**
- Modify: `firmware/src/render.cpp`

- [ ] **Step 1: Fill in the "no data yet" branch in `renderTick`**

Task 15 already left a stub: `if (!s.valid) { displayCommit(); return; }`. Replace that line with:

```cpp
if (!s.valid) {
  d->setTextSize(2);
  d->setTextColor(INK);
  d->setCursor(80, 130);
  d->print("Waiting for first sync...");
  displayCommit();
  return;
}
```

- [ ] **Step 2: Flash, power on with no posts; observe waiting screen. Then post a fixture; observe full layout.**

- [ ] **Step 3: Commit**

```bash
git add firmware/src/render.cpp
git commit -m "firmware: waiting-for-first-sync screen"
```

### Task 18: Wi-Fi disconnect indicator polish

**Files:**
- Modify: `firmware/src/render.cpp`

- [ ] **Step 1: Replace the placeholder `!` with a clearer marker**

In `drawHeader`, when `!wifi_ok`:

```cpp
d->setCursor(330, 4);
d->print("WiFi?");
```

- [ ] **Step 2: Flash. Disconnect Wi-Fi (e.g., disable router temporarily); after a few seconds the header should show "WiFi?". Re-enable; indicator clears within next render tick.**

- [ ] **Step 3: Commit**

```bash
git add firmware/src/render.cpp
git commit -m "firmware: clearer Wi-Fi-down indicator"
```

---

## Phase D — Integration & soak

### Task 19: End-to-end smoke from Mac

**Files:** none

- [ ] **Step 1: Manually invoke the real script**

```bash
cd mac
./push-usage.sh
```

Expected: device updates within ~1 s; serial log shows `[api] state updated`.

- [ ] **Step 2: Verify all fixture edge cases visually**

```bash
for f in fixtures/edge-cases/*.json; do echo "=== $f ==="; ./test-push.sh "$f"; sleep 5; done
```

- [ ] **Step 3: Note any rendering anomalies; fix in render.cpp; re-test; commit any fixes with descriptive messages**

### Task 20: Install launchd job + 24 h soak

**Files:**
- Modify: `mac/com.rock.ai-usage-push.plist` (path adjustments if needed)
- Modify: `README.md` (add Troubleshooting section if any issues surfaced)

- [ ] **Step 1: Install plist**

```bash
cp mac/com.rock.ai-usage-push.plist ~/Library/LaunchAgents/
launchctl load ~/Library/LaunchAgents/com.rock.ai-usage-push.plist
launchctl list | grep ai-usage-push
```

Expected: a line with the label and a recent exit code.

- [ ] **Step 2: Tail the log to confirm 60-second cadence**

```bash
tail -f ~/Library/Logs/ai-usage-push.log
```

Expected: one entry per minute; HTTP 200 lines from curl in stderr.

- [ ] **Step 3: Soak for 24 h. Observe**

- Does the device stay live?
- Does Mac sleep produce STALE correctly?
- Does Mac wake-from-sleep recover within one minute?
- Any unexpected drift in counters?

- [ ] **Step 4: Document any issues + fixes; commit**

```bash
git commit -am "docs: troubleshooting notes from soak"
```

- [ ] **Step 5: Final sanity check + push**

```bash
git status
git log --oneline
git push origin main
```

---

## Self-review checklist (run before handoff)

- [x] Spec §1 — tokens-as-soul, time-progress bars, no fake quota: enforced by Tasks 5 (no `percent`/`limit_tokens` in output) and 15 (bar formula uses time, big number is tokens).
- [x] Spec §2 goals — block + weekly visibility plus time progress: Tasks 5, 14–16.
- [x] Spec §3 hard constraints — no API (Tasks 1, 5), no Mac listener (Tasks 5, 7 — `curl` outbound only), no device-initiated traffic (Tasks 11, 12 — server-only).
- [x] Spec §4 architecture — Tasks 5–8 (Mac), 9–18 (firmware).
- [x] Spec §5 components — every file in §5.1 has a creating task; vendoring in Task 13.
- [x] Spec §6 wire schema — Task 5 emits the new shape (`block_5h` rename, `started_at`, `cost_usd`, `messages`, `burn_rate_tpm`); Task 12 parses with required-field validation; bats and Unity tests assert the shape.
- [x] Spec §7 revised layout — Tasks 14, 15, 16 implement tokens-as-big-number + time-progress bar + footer.
- [x] Spec §8 data flow — exercised end-to-end in Task 19.
- [x] Spec §9 error handling — STALE in Task 16, waiting screen in Task 17, Wi-Fi indicator in Task 18, malformed JSON in Task 12.
- [x] Spec §10 testing — bats in Tasks 4–6, native unit tests in Task 12, manual fixture POSTing in Task 8, soak in Task 20.
- [x] Spec §11 risks — Tasks 1+2 closed the ccusage and LCD-driver risks (notes referenced); §11.2 weekly-clock approximation lands in Task 5; §11.4 mDNS validated in Task 11.
