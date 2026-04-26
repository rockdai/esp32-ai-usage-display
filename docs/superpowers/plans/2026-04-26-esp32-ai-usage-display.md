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
- Create: `mac/fixtures/sample-blocks.json`
- Create: `mac/test/aggregate.bats`

The aggregation core is the riskiest pure-logic piece. We test it with hand-crafted fixtures that simulate `ccusage`'s output (per Task 1's findings).

- [ ] **Step 1: Write a fixture representing 5h-window data**

Create `mac/fixtures/sample-blocks.json` based on the actual structure recorded in Task 1's notes. Example shape (adjust to match real ccusage output):

```json
{
  "now_epoch": 1745673120,
  "blocks": [
    {"start_epoch": 1745655120, "end_epoch": 1745673120, "tokens": 342000}
  ],
  "weekly": {
    "start_epoch": 1745222400, "end_epoch": 1745827200, "tokens": 1800000
  },
  "today": {"tokens": 5200000, "sessions": 14}
}
```

- [ ] **Step 2: Write the failing test for the aggregation function**

Create `mac/test/aggregate.bats`:

```bash
#!/usr/bin/env bats

setup() {
  PROJECT_ROOT="$(cd "$BATS_TEST_DIRNAME/.." && pwd)"
  PUSH="$PROJECT_ROOT/push-usage.sh"
  FIXTURE="$PROJECT_ROOT/fixtures/sample-blocks.json"
}

@test "aggregates fixture into wire-schema JSON" {
  run "$PUSH" --input "$FIXTURE" --emit-only
  [ "$status" -eq 0 ]
  ts=$(echo "$output" | jq -r '.ts')
  pct=$(echo "$output" | jq -r '.window_5h.percent')
  [ "$ts" = "1745673120" ]
  [ "$pct" = "47" ]
}

@test "weekly percent computed against plan limit" {
  run "$PUSH" --input "$FIXTURE" --emit-only
  pct=$(echo "$output" | jq -r '.weekly.percent')
  [ "$pct" = "18" ]
}

@test "today fields passed through" {
  run "$PUSH" --input "$FIXTURE" --emit-only
  toks=$(echo "$output" | jq -r '.today.tokens')
  sess=$(echo "$output" | jq -r '.today.sessions')
  [ "$toks" = "5200000" ]
  [ "$sess" = "14" ]
}
```

- [ ] **Step 3: Run tests and confirm they fail**

Run: `bats mac/test/aggregate.bats`
Expected: 3 failures, message about `push-usage.sh: not found` or similar.

- [ ] **Step 4: Write the minimal `push-usage.sh`**

Create `mac/push-usage.sh` (use the actual ccusage shape from Task 1; this is the template):

```bash
#!/usr/bin/env bash
set -euo pipefail

# Plan limits — confirmed in Task 1 (docs/superpowers/notes/2026-04-26-ccusage-investigation.md)
LIMIT_5H=720000
LIMIT_WEEKLY=10000000
PLAN_NAME="Max 5x"

INPUT=""
EMIT_ONLY=0

while [ $# -gt 0 ]; do
  case "$1" in
    --input)     INPUT="$2"; shift 2 ;;
    --emit-only) EMIT_ONLY=1; shift ;;
    *) echo "unknown flag: $1" >&2; exit 64 ;;
  esac
done

read_fixture() {
  cat "$INPUT"
}

aggregate() {
  jq -c --argjson l5h "$LIMIT_5H" --argjson lw "$LIMIT_WEEKLY" --arg plan "$PLAN_NAME" '
    {
      ts: .now_epoch,
      plan: $plan,
      window_5h: {
        percent: ((.blocks | map(.tokens) | add // 0) * 100 / $l5h | floor),
        used_tokens: (.blocks | map(.tokens) | add // 0),
        limit_tokens: $l5h,
        resets_at: (.blocks | map(.end_epoch) | max // .now_epoch)
      },
      weekly: {
        percent: (.weekly.tokens * 100 / $lw | floor),
        used_tokens: .weekly.tokens,
        limit_tokens: $lw,
        resets_at: .weekly.end_epoch
      },
      today: {
        tokens: .today.tokens,
        sessions: .today.sessions
      }
    }
  '
}

if [ -n "$INPUT" ]; then
  payload="$(read_fixture | aggregate)"
else
  payload="$(npx ccusage@latest --json | aggregate)"
fi

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

- [ ] **Step 5: Make script executable**

Run: `chmod +x mac/push-usage.sh`

- [ ] **Step 6: Run tests; expect them to pass**

Run: `bats mac/test/aggregate.bats`
Expected: `3 tests, 0 failures`.

- [ ] **Step 7: Commit**

```bash
git add mac/push-usage.sh mac/fixtures/sample-blocks.json mac/test/aggregate.bats
git commit -m "mac: push-usage.sh aggregation with bats fixtures"
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
  echo "$output" | jq -e 'has("ts") and has("window_5h") and has("weekly")'
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

Create `mac/fixtures/edge-cases/zero.json`:

```json
{"ts":1745673120,"plan":"Max 5x","window_5h":{"percent":0,"used_tokens":0,"limit_tokens":720000,"resets_at":1745682000},"weekly":{"percent":0,"used_tokens":0,"limit_tokens":10000000,"resets_at":1746057600},"today":{"tokens":0,"sessions":0}}
```

Create `mac/fixtures/edge-cases/full.json`:

```json
{"ts":1745673120,"plan":"Max 5x","window_5h":{"percent":100,"used_tokens":720000,"limit_tokens":720000,"resets_at":1745682000},"weekly":{"percent":100,"used_tokens":10000000,"limit_tokens":10000000,"resets_at":1746057600},"today":{"tokens":42000000,"sessions":99}}
```

Create `mac/fixtures/edge-cases/missing-today.json`:

```json
{"ts":1745673120,"plan":"Max 5x","window_5h":{"percent":47,"used_tokens":342000,"limit_tokens":720000,"resets_at":1745682000},"weekly":{"percent":18,"used_tokens":1800000,"limit_tokens":10000000,"resets_at":1746057600}}
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
  uint8_t  pct_5h = 0;
  uint64_t tok_5h_used = 0;
  uint64_t tok_5h_limit = 0;
  uint32_t reset_5h = 0;
  uint8_t  pct_weekly = 0;
  uint64_t tok_weekly_used = 0;
  uint64_t tok_weekly_limit = 0;
  uint32_t reset_weekly = 0;
  uint64_t today_tokens = 0;
  uint32_t today_sessions = 0;
  bool     today_present = false;
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
  if (!doc["window_5h"]["percent"].is<int>()) return false;
  if (!doc["weekly"]["percent"].is<int>()) return false;

  out.ts = doc["ts"];
  strlcpy(out.plan, doc["plan"] | "", sizeof(out.plan));
  out.pct_5h           = doc["window_5h"]["percent"];
  out.tok_5h_used      = doc["window_5h"]["used_tokens"]  | 0;
  out.tok_5h_limit     = doc["window_5h"]["limit_tokens"] | 0;
  out.reset_5h         = doc["window_5h"]["resets_at"]    | 0;
  out.pct_weekly       = doc["weekly"]["percent"];
  out.tok_weekly_used  = doc["weekly"]["used_tokens"]  | 0;
  out.tok_weekly_limit = doc["weekly"]["limit_tokens"] | 0;
  out.reset_weekly     = doc["weekly"]["resets_at"]    | 0;

  if (!doc["today"].isNull()) {
    out.today_tokens   = doc["today"]["tokens"]   | 0;
    out.today_sessions = doc["today"]["sessions"] | 0;
    out.today_present  = true;
  } else {
    out.today_present  = false;
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
    "ts": 1745673120, "plan": "Max 5x",
    "window_5h": {"percent": 47, "used_tokens": 342000, "limit_tokens": 720000, "resets_at": 1745682000},
    "weekly":    {"percent": 18, "used_tokens": 1800000, "limit_tokens": 10000000, "resets_at": 1746057600},
    "today":     {"tokens": 5200000, "sessions": 14}
  })";
  UsageData u;
  TEST_ASSERT_TRUE(parseUsageJson(body, u));
  TEST_ASSERT_EQUAL_UINT32(1745673120, u.ts);
  TEST_ASSERT_EQUAL_STRING("Max 5x", u.plan);
  TEST_ASSERT_EQUAL_UINT8(47, u.pct_5h);
  TEST_ASSERT_EQUAL_UINT8(18, u.pct_weekly);
  TEST_ASSERT_TRUE(u.today_present);
  TEST_ASSERT_EQUAL_UINT64(5200000, u.today_tokens);
}

void test_missing_today_is_ok() {
  const char* body = R"({
    "ts": 1, "plan": "x",
    "window_5h":{"percent":0,"used_tokens":0,"limit_tokens":1,"resets_at":0},
    "weekly":   {"percent":0,"used_tokens":0,"limit_tokens":1,"resets_at":0}
  })";
  UsageData u;
  TEST_ASSERT_TRUE(parseUsageJson(body, u));
  TEST_ASSERT_FALSE(u.today_present);
}

void test_malformed_rejects() {
  UsageData u;
  TEST_ASSERT_FALSE(parseUsageJson("not json", u));
  TEST_ASSERT_FALSE(parseUsageJson("{}", u));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_valid_payload);
  RUN_TEST(test_missing_today_is_ok);
  RUN_TEST(test_malformed_rejects);
  return UNITY_END();
}
```

- [ ] **Step 6: Run native tests**

Run: `pio test -e native`
Expected: 3 tests pass.

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
  Serial.printf("[api] state updated pct_5h=%u pct_w=%u\n", parsed.pct_5h, parsed.pct_weekly);
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
Run on Mac: `cd mac && ./test-push.sh fixtures/edge-cases/zero.json`
Expected (device serial): `[api] state updated pct_5h=0 pct_w=0` and `200 OK` returned to curl.

- [ ] **Step 9: Commit**

```bash
git add firmware/src/state.h firmware/src/api.h firmware/src/api.cpp \
        firmware/test/test_api/test_api.cpp firmware/src/main.cpp \
        firmware/platformio.ini
git commit -m "firmware: HTTP /data endpoint + native-tested JSON parser"
```

### Task 13: LovyanGFX panel init + draw "OK"

**Files:**
- Create: `firmware/src/display.h`
- Create: `firmware/src/display.cpp`
- Modify: `firmware/src/main.cpp`

- [ ] **Step 1: Adapt the panel config from Task 2's notes into `display.cpp`**

Skeleton (real values from Task 2's notes go into the constructor body):

```cpp
// firmware/src/display.cpp
#include "display.h"
#include <LovyanGFX.hpp>

class PanelRLCD42 : public lgfx::LGFX_Device {
  lgfx::Panel_XXX     _panel;     // <-- driver class from Task 2
  lgfx::Bus_SPI       _bus;
public:
  PanelRLCD42() {
    auto bcfg = _bus.config();
    bcfg.spi_host  = SPI2_HOST;
    bcfg.spi_mode  = 0;
    bcfg.freq_write = 40000000;
    bcfg.pin_sclk  = 0;  // <-- from Task 2
    bcfg.pin_mosi  = 0;
    bcfg.pin_miso  = -1;
    bcfg.pin_dc    = 0;
    _bus.config(bcfg);
    _panel.setBus(&_bus);

    auto pcfg = _panel.config();
    pcfg.pin_cs   = 0;   // <-- from Task 2
    pcfg.pin_rst  = 0;
    pcfg.pin_busy = -1;
    pcfg.panel_width  = 300;
    pcfg.panel_height = 400;
    pcfg.offset_rotation = 1;   // landscape
    _panel.config(pcfg);
    setPanel(&_panel);
  }
};

static PanelRLCD42 lcd;

void displayInit() {
  lcd.init();
  lcd.setRotation(1);            // 400×300 landscape
  lcd.fillScreen(TFT_WHITE);
  lcd.setTextColor(TFT_BLACK);
  lcd.setTextSize(3);
  lcd.setCursor(20, 20);
  lcd.print("OK");
}

LGFX& displayDevice() { return lcd; }
```

`display.h`:

```cpp
// firmware/src/display.h
#pragma once
#include <LovyanGFX.hpp>
void displayInit();
LGFX& displayDevice();
```

- [ ] **Step 2: Call `displayInit()` from `setup()`**

Add `#include "display.h"` and call `displayInit();` in `setup()` before `connectWifi()`.

- [ ] **Step 3: Flash, observe**

Run: `pio run -t upload && pio device monitor`
Expected: panel shows "OK" in the upper-left.

- [ ] **Step 4: Commit**

```bash
git add firmware/src/display.h firmware/src/display.cpp firmware/src/main.cpp
git commit -m "firmware: LovyanGFX panel init draws OK"
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
#include <time.h>

static LGFX* d = nullptr;

void renderInit() { d = &displayDevice(); }

static void drawHeader(const UsageData& s, bool wifi_ok) {
  d->fillRect(0, 0, 400, 24, TFT_WHITE);
  d->setCursor(8, 4);
  d->setTextSize(2);
  d->setTextColor(TFT_BLACK);
  d->printf("CLAUDE CODE  %s", s.plan[0] ? s.plan : "");
  if (!wifi_ok) {
    d->setCursor(380, 4);
    d->print("!");          // simple Wi-Fi-down marker
  }
  // separator line
  d->drawLine(0, 24, 400, 24, TFT_BLACK);
}

void renderTick(const UsageData& s, bool stale, bool wifi_ok, uint32_t ms_since_post) {
  (void)stale; (void)ms_since_post;          // used in later tasks
  d->fillScreen(TFT_WHITE);
  drawHeader(s, wifi_ok);
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

### Task 15: Render — 5h and weekly sections

**Files:**
- Modify: `firmware/src/render.cpp`

- [ ] **Step 1: Add bar+number drawing functions**

```cpp
// at top of render.cpp
static void drawWindow(int y, const char* label, uint8_t pct,
                       uint64_t used, uint64_t limit, uint32_t resets_at, uint32_t now) {
  d->setTextSize(2);
  d->setCursor(8, y);
  d->print(label);

  // big percent number, right-aligned-ish
  d->setTextSize(4);
  d->setCursor(280, y - 6);
  d->printf("%3u%%", pct);

  // bar
  int barY = y + 28;
  d->drawRect(8, barY, 384, 16, TFT_BLACK);
  int fill = (pct > 100 ? 100 : pct) * 380 / 100;
  d->fillRect(10, barY + 2, fill, 12, TFT_BLACK);

  // meta line
  d->setTextSize(1);
  d->setCursor(8, barY + 22);
  d->printf("%llu / %llu tokens", (unsigned long long)used, (unsigned long long)limit);

  if (resets_at > now) {
    uint32_t rem = resets_at - now;
    d->setCursor(220, barY + 22);
    d->printf("resets in %lus", (unsigned long)rem);  // refined later
  }
}
```

(Reset countdown formatting will be cleaned up in Task 16.)

- [ ] **Step 2: Call from `renderTick`**

```cpp
void renderTick(const UsageData& s, bool stale, bool wifi_ok) {
  d->fillScreen(TFT_WHITE);
  drawHeader(s, wifi_ok);
  drawWindow( 36, "5H WINDOW", s.pct_5h,     s.tok_5h_used,     s.tok_5h_limit,     s.reset_5h,     s.ts);
  drawWindow(140, "WEEKLY",    s.pct_weekly, s.tok_weekly_used, s.tok_weekly_limit, s.reset_weekly, s.ts);
}
```

- [ ] **Step 3: Flash + post `mac/fixtures/edge-cases/zero.json` then `full.json`, photograph each**

Verify both bars render at 0% and 100%; numbers display sensibly; layout fits within 300 px height.

- [ ] **Step 4: Commit**

```bash
git add firmware/src/render.cpp
git commit -m "firmware: render 5h and weekly sections"
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
  d->drawLine(0, y - 4, 400, y - 4, TFT_BLACK);
  d->setTextSize(1);
  d->setCursor(8, y);
  if (s.today_present) {
    d->printf("Today: %llu tok / %u sess",
              (unsigned long long)s.today_tokens, s.today_sessions);
  }
  d->setCursor(280, y);
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

- [ ] **Step 1: Detect "no data yet"**

In `renderTick`, immediately after `d->fillScreen(TFT_WHITE); drawHeader(s, wifi_ok);` and before the `drawWindow` calls, insert:

```cpp
if (!s.valid) {
  d->setTextSize(2);
  d->setCursor(80, 130);
  d->print("Waiting for first sync...");
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

- [x] Spec §2 goals — primary 5h + weekly + secondary tokens: covered by Tasks 5, 14–16
- [x] Spec §3 hard constraints — no API (Tasks 1, 5), no Mac listener (Tasks 5, 7), no device-initiated traffic (Tasks 11, 12 — server-only)
- [x] Spec §4 architecture — Tasks 5–8 (Mac), 9–18 (firmware)
- [x] Spec §5 components — every file in §5.1 has a creating task
- [x] Spec §6 wire schema — Task 5 emits, Task 12 parses, native test asserts shape
- [x] Spec §7 layout — Tasks 14–17
- [x] Spec §8 data flow — exercised end-to-end in Task 19
- [x] Spec §9 error handling — STALE in Task 16, waiting screen in Task 17, Wi-Fi indicator in Task 18, malformed JSON in Task 12
- [x] Spec §10 testing — bats in Tasks 4–6, native unit tests in Task 12, soak in Task 20
- [x] Spec §11 risks — ccusage validated in Task 1, plan limits in Task 1, LCD driver in Task 2, mDNS in Task 11
