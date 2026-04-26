# ESP32 AI Usage Display — Design

**Date**: 2026-04-26
**Hardware**: Waveshare ESP32-S3-RLCD-4.2 (300×400 reflective LCD, no touch)
**Scope**: v1 — Claude Code only. Codex CLI is explicitly deferred.

## 1. Purpose

A desk-mounted, always-on, dimly-lit-room-friendly display that shows the user's
**Claude Code 5-hour rolling window** quota and **weekly window** quota at a
glance, with token usage as a secondary metric.

The display is driven by data parsed locally from the user's Mac — no calls to
Anthropic's API, no listening services on the Mac.

## 2. Goals & non-goals

### Goals

- See current 5-hour and weekly quota burn at a glance from across the desk.
- Update at most 60 s after Claude usage actually changes.
- Survive Mac sleep / Wi-Fi flap / device reboot without manual intervention.
- One-time setup; no maintenance interaction.

### Non-goals (v1)

- Codex CLI display (deferred — see §11).
- Use of the device's KEY button (intentionally unused; no force-refresh).
- Use of the device's microphones, RTC alarms, SD card, or audio codec.
- Cloud relay / off-LAN visibility.
- Color theming on the reflective LCD beyond plain black-on-paper-tone.

## 3. Hard constraints

These came from the user during brainstorming and are non-negotiable:

1. **No official AI provider APIs.** Claude usage data must come from local logs
   only (parse `~/.claude/projects/**/*.jsonl` and/or use the community
   `ccusage` tool).
2. **No listening sockets on the Mac.** The Mac is allowed to run scheduled
   commands (launchd) and make outbound HTTP requests, but never to bind a
   listening port. This rules out a Mac-side HTTP server, MQTT broker, etc.
3. **No device-initiated network requests.** The ESP32 only listens; it never
   originates outbound traffic. (Implication: there is no force-refresh
   mechanism. Latency floor = push interval.)

## 4. Architecture

```
┌──────────── Mac ────────────────────────────────────────┐
│ launchd (StartInterval=60s)                             │
│   └─> mac/push-usage.sh                                 │
│         ├─ npx ccusage  (or direct jsonl parse)         │
│         ├─ aggregate to wire schema                     │
│         └─ curl -X POST http://ai-usage-display.local   │
│                            /data --json @-              │
└─────────────────────────────────────────────────────────┘
                 │ HTTP/JSON, LAN, mDNS hostname
                 ▼
┌──────────── ESP32-S3-RLCD-4.2 ──────────────────────────┐
│ firmware (Arduino + LovyanGFX, built via PlatformIO)    │
│   ├─ Wi-Fi STA  (creds from gitignored secrets.h)       │
│   ├─ mDNS broadcast  ai-usage-display.local             │
│   ├─ HTTP server   POST /data → state                   │
│   └─ render loop   1 Hz tick → B1 layout (400×300)      │
└─────────────────────────────────────────────────────────┘
```

Communication is strictly **one-way push** Mac → ESP32. Either side can fail
without corrupting the other; the display merely goes stale.

## 5. Components

### 5.1 Repo layout

```
esp32-ai-usage-display/
├── mac/
│   ├── push-usage.sh                    # 60s job entry point
│   ├── com.rock.ai-usage-push.plist     # launchd agent
│   ├── secrets.env.example
│   └── secrets.env                      # gitignored: HOST=ai-usage-display.local
├── firmware/
│   ├── platformio.ini
│   ├── src/
│   │   ├── main.cpp                     # setup() + loop()
│   │   ├── api.cpp / api.h              # POST /data handler
│   │   ├── display.cpp / display.h      # LovyanGFX panel init
│   │   ├── render.cpp / render.h        # B1 layout drawing
│   │   ├── state.h                      # UsageData struct + mutex
│   │   ├── secrets.h                    # gitignored
│   │   └── secrets.h.example
│   └── README.md
├── docs/superpowers/specs/
│   └── 2026-04-26-esp32-ai-usage-display-design.md  (this file)
├── .gitignore
└── README.md
```

### 5.2 Mac side

- **launchd plist**: `RunAtLoad=true`, `StartInterval=60`, `StandardOutPath` and
  `StandardErrorPath` to `~/Library/Logs/ai-usage-push.log`.
- **push-usage.sh** (bash):
  1. Resolve plan limits from constants at top of script (Max 5x).
  2. Run `ccusage` (or fallback inline jsonl parse) to get token usage events.
  3. Aggregate the trailing 5-hour window and the current weekly window
     (per Anthropic's policy — to be confirmed during implementation, see
     §11.2); compute percent against plan limits; compute today's totals;
     compute the next reset epoch for each window.
  4. Emit JSON conforming to §6.
  5. POST to `http://${HOST}/data`, retry once on transient curl error, log
     non-zero exits.
- **secrets.env**: only `HOST=ai-usage-display.local`. Sourced by the script.

### 5.3 ESP32 firmware

- **PlatformIO** with `framework = arduino`, board configured for
  `esp32-s3-devkitc-1` plus the N16R8 chip variant. Libraries: `LovyanGFX`,
  `ArduinoJson`, `WebServer`, `ESPmDNS`.
- **secrets.h**: `WIFI_SSID`, `WIFI_PWD` only. Gitignored.
- **main.cpp**: connect Wi-Fi → start mDNS as `ai-usage-display` → start HTTP
  server on `:80` with `POST /data` → enter render loop.
- **api.cpp**: parses request body with ArduinoJson; validates required fields
  (`ts`, `window_5h.percent`, `weekly.percent`); on success copies into the
  `UsageData` global behind a mutex, sets `dirty=true`, returns `200`. On
  malformed input returns `400` and leaves state untouched.
- **state.h**: `UsageData { uint32_t ts; uint8_t pct_5h; uint8_t pct_weekly;
  uint64_t tok_5h; uint64_t tok_weekly_used; … char plan[16]; }`. A boolean
  `dirty` flag plus a mutex. A separate `uint32_t last_post_millis` tracks when
  the last successful POST landed (for STALE detection — see §8).
- **display.cpp**: LovyanGFX panel init (driver model TBD against Waveshare's
  Arduino sample; copy from their reference repo). Rotation is set to landscape
  so the framebuffer is 400 wide × 300 tall.
- **render.cpp**: draws the B1 layout (§7). Implements full-frame redraws on
  dirty; checks STALE every tick.

## 6. Wire schema

Body of `POST /data`, `Content-Type: application/json`. All times are epoch
seconds (UTC).

```json
{
  "ts":        1745673120,
  "plan":      "Max 5x",
  "window_5h": {
    "percent":      47,
    "used_tokens":  342000,
    "limit_tokens": 720000,
    "resets_at":    1745682000
  },
  "weekly": {
    "percent":      18,
    "used_tokens":  1800000,
    "limit_tokens": 10000000,
    "resets_at":    1746057600
  },
  "today": {
    "tokens":   5200000,
    "sessions": 14
  }
}
```

Required: `ts`, `plan`, `window_5h.percent`, `weekly.percent`. Other fields are
displayed when present and rendered as `—` when absent (forward-compatible).

## 7. Layout (B1, landscape 400×300)

```
┌──────────────────────────────────────────────────┐  ← 400 px
│ CLAUDE CODE · MAX 5X              14:32          │  header (border-bottom)
├──────────────────────────────────────────────────┤
│ 5H WINDOW                              47%       │  big number
│ ████████████████░░░░░░░░░░░░░░░░░░░░░░           │  full-width bar
│ 342K / 720K tokens          resets 16:45 · 2h13m │  meta line
│                                                  │
│ WEEKLY                                 18%       │
│ ███████░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░           │
│ 1.8M / 10M tokens         resets Sun 00:00 · 4d  │
├──────────────────────────────────────────────────┤
│ Today: 5.2M tok · 14 sessions     Updated 14:32  │
└──────────────────────────────────────────────────┘  ← 300 px
```

- Monospace, heavy weight, high contrast (RLCD favors block shapes over
  hairlines).
- Colors: black on the panel's natural off-white. No grayscale gradients in
  bars — solid fill. Color use deliberately deferred until v2 once we measure
  how the panel actually reproduces tints.
- All text sizes chosen so the two big percentages and both bars are
  legible from ~1.5 m.

## 8. Data flow (one cycle)

| Time | Event |
|---|---|
| T+0 s | launchd fires `push-usage.sh`. |
| T+~300 ms | ccusage returns; aggregation done; JSON ready. |
| T+~400 ms | curl POST completes; ESP32 stores `state`, sets `dirty`, returns 200. `last_post_millis = millis()`. |
| every 1 s | render loop: if `dirty`, full redraw; clear `dirty`. Always recompute `(millis() - last_post_millis)`; if > 300 000 ms, draw `STALE` indicator in the footer. |
| T+60 s | next launchd tick. |

Reset countdowns (`2h 13m`, `4d 9h`, etc.) are recomputed from `resets_at - ts`
plus elapsed-on-device time, so they tick down between pushes without needing
fresh data.

## 9. Error handling

| Scenario | Behavior |
|---|---|
| First boot, no POST yet | Center text "Waiting for first sync…"; small WiFi indicator if connected. |
| Mac asleep / off | Last data persists; footer shows `STALE · last update HH:MM` after 5 min. |
| Wi-Fi drops (device side) | Header right gets `⚠ WiFi`; STA reconnect loop runs in background. |
| ccusage fails / wrong format | Script exits non-zero; nothing posted; device drifts to STALE. Errors logged to `~/Library/Logs/ai-usage-push.log`. |
| Malformed POST body | ESP32 returns 400; existing state untouched. |
| ESP32 reboots | After Wi-Fi reconnect, returns to "Waiting for first sync…" until next minute. |
| NTP not synced on device | Doesn't matter — STALE uses `millis()` deltas; reset countdowns use server-supplied epoch. |

## 10. Testing & verification

1. **Mac script** (`mac/push-usage.sh`):
   - Hand-curated `fixtures/ccusage-sample.json`. Script accepts `--input FILE`
     to bypass real ccusage execution.
   - Bats-style assertions that the resulting JSON matches §6 (percent values,
     token sums, reset times).
2. **Firmware** rendering:
   - `mac/test-push.sh` posts hand-crafted fixtures to a real device. Cases:
     0%, 50%, 100%, missing `today`, plan name overflow, very large token
     counts, future `resets_at`.
   - Manual visual confirmation; photograph at oblique angle to mimic real
     viewing.
3. **End-to-end soak**: real device, real account, 24 h. Verify push cadence
   (one entry/min in log), STALE behavior on Mac sleep, recovery after Wi-Fi
   loss.
4. **Out of scope**: pixel-level rendering tests, launchd scheduling
   simulation.

## 11. Risks & open questions

1. **`ccusage` capability gap.** I have not verified that ccusage natively
   exposes the 5-hour rolling window and weekly window aggregates. If it does
   not, `push-usage.sh` falls back to: walk `~/.claude/projects/**/*.jsonl`,
   sum input+output token deltas in the trailing windows. This is the first
   thing to validate during implementation.
2. **Plan-limit constants.** Anthropic's exact token caps for "Max 5x" on the
   5-hour and weekly windows must be confirmed (from public docs or empirical
   observation) before percentages are meaningful.
3. **LCD driver model**. Waveshare's wiki references the driver but doesn't
   give a one-line LovyanGFX panel config. Implementation step: copy and adapt
   from their official Arduino sample.
4. **mDNS reliability** on the user's home network: most macOS setups resolve
   `*.local` natively; if not, fallback is a hardcoded IP via a DHCP
   reservation on the router.

## 12. Future (deferred, not in v1)

- Codex CLI section (second screen swapped via the currently-unused KEY
  button, or a horizontal split).
- Color theming once we calibrate to the actual panel.
- Battery operation with deep sleep + a different (likely pull-based)
  architecture.
- Off-LAN visibility via a cloud relay.
