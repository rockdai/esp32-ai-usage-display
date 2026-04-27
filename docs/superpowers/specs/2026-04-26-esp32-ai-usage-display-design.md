# ESP32 AI Usage Display — Design

**Date**: 2026-04-26
**Hardware**: Waveshare ESP32-S3-RLCD-4.2 (300×400 reflective LCD, no touch)
**Scope**: v1 — Claude Code only. Codex CLI is explicitly deferred.

## 1. Purpose

A desk-mounted, always-on, dimly-lit-room-friendly display that shows, at a
glance, the user's **Claude Code 5-hour-block token usage** and **weekly-window
token usage** — with the absolute token count as the dominant figure on each
row, plus supporting cost / message / burn-rate fields.

The display is driven by data parsed locally from the user's Mac — no calls to
Anthropic's API, no listening services on the Mac.

> **Why no quota percentages?** Anthropic does not publish exact token caps for
> consumer plans (their published limits are in messages or model-hours, with
> wide ranges that vary by conversation cache hit rate). Computing
> `tokens_used / fabricated_cap × 100%` would be a fiction. The device shows
> only numbers we can derive truthfully.

## 2. Goals & non-goals

### Goals

- See current 5-hour-block token usage and weekly-window token usage at a
  glance from across the desk.
- Show how far through each window we are **in time** (block elapsed / 5 h;
  week elapsed / 7 d), as an honest progress indicator.
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
  1. Run `ccusage blocks --active --json` for the current 5-hour block — gives
     `started_at`, `resets_at` (= started_at + 5 h), `used_tokens`, `cost_usd`,
     `burn_rate_tpm`, `messages` directly. Empty `blocks: []` means "idle ≥5 h
     since last session" — emit zeros and a synthetic block boundary.
  2. Compute the **weekly window** locally: scan `~/.claude/projects/**/*.jsonl`
     for `type=="user"` events with `timestamp >= now - 168 h`; the minimum
     timestamp is `weekly.started_at`; `weekly.resets_at = started_at + 7 d`;
     `used_tokens` is summed from `ccusage daily --json` for the days inside
     this window. Cost and message counts likewise.
  3. Compute today's totals from `ccusage daily --json` (current local date).
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
  (`ts`, `block_5h.used_tokens`, `block_5h.started_at`, `block_5h.resets_at`,
  `weekly.used_tokens`, `weekly.started_at`, `weekly.resets_at`); on success
  copies into the `UsageData` global behind a mutex, sets `dirty=true`,
  returns `200`. On malformed input returns `400` and leaves state untouched.
- **state.h**: `UsageData { uint32_t ts; char plan[16]; uint64_t tok_5h;
  uint32_t started_5h; uint32_t reset_5h; double cost_5h_usd; uint32_t msgs_5h;
  uint32_t burn_tpm; uint64_t tok_weekly; uint32_t started_weekly;
  uint32_t reset_weekly; double cost_weekly_usd; uint32_t msgs_weekly;
  uint64_t tok_today; uint32_t msgs_today; double cost_today_usd; bool valid;
  }`. A boolean `dirty` flag plus a mutex. A separate `uint32_t last_post_millis`
  tracks when the last successful POST landed (for STALE detection — see §8).
- **display.cpp**: LovyanGFX is **not** used as a panel-class wrapper because
  the panel is a Sitronix ST7305 mono reflective LCD (1 bpp, non-linear 4×2
  tile framebuffer) and no upstream LovyanGFX driver exists. We vendor
  Waveshare's `display_bsp.{h,cpp}` for the low-level chip init and use
  `LGFX_Sprite` + `DisplayPort` to compose frames in a 1-bit canvas, then blit.
  See `docs/superpowers/notes/2026-04-26-lcd-driver.md`. Rotation: software
  rotation in the canvas → 400 × 300 landscape.
- **render.cpp**: draws the B1 layout (§7). Implements full-frame redraws on
  dirty; checks STALE every tick.

## 6. Wire schema

Body of `POST /data`, `Content-Type: application/json`. All times are epoch
seconds (UTC).

```json
{
  "ts":        1745673120,
  "plan":      "Max 5x",
  "block_5h": {
    "used_tokens":   1017862,
    "started_at":    1745655120,
    "resets_at":     1745673120,
    "cost_usd":      1.81,
    "messages":      11,
    "burn_rate_tpm": 82684
  },
  "weekly": {
    "used_tokens": 5400000,
    "started_at":  1745222400,
    "resets_at":   1745827200,
    "cost_usd":    24.50,
    "messages":    47
  },
  "today": {
    "tokens":   2100000,
    "messages": 14,
    "cost_usd": 5.62
  }
}
```

**Required** (POST is rejected with 400 if missing): `ts`, `plan`,
`block_5h.used_tokens`, `block_5h.started_at`, `block_5h.resets_at`,
`weekly.used_tokens`, `weekly.started_at`, `weekly.resets_at`.

**Optional** (rendered when present, omitted gracefully otherwise):
`block_5h.{cost_usd,messages,burn_rate_tpm}`, `weekly.{cost_usd,messages}`,
the entire `today` object, and any future fields (forward-compatible).

**Notes:**
- `started_at` is the absolute epoch at which the window began (5h block: aligned
  to clock boundary; weekly: the user's first session timestamp in the trailing
  168 h, scanned from local `~/.claude/projects/**/*.jsonl`).
- The device computes time-progress as `(now − started_at) / (resets_at − started_at)`
  and tokens as the bare `used_tokens` figure. There is no quota / percent /
  limit field — by design (see §1).
- All field names use lower-snake-case; epoch fields are `uint32_t`-safe through
  2106; token counts are `uint64_t`-safe.

## 7. Layout (B1-revised, landscape 400×300)

```
┌──────────────────────────────────────────────────┐  ← 400 px
│ CLAUDE CODE · MAX 5X              14:32          │  header (border-bottom)
├──────────────────────────────────────────────────┤
│ 5H BLOCK                              1.0M       │  big number = used tokens
│ ████████████████░░░░░░░░░░░░░░░░░░░░░░           │  bar = TIME progress (block elapsed / 5h)
│ $1.81 · 11 msg · 83K/min        resets in 4h30m  │  meta line
│                                                  │
│ WEEKLY                                5.4M       │
│ ███░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░             │  bar = TIME progress (week elapsed / 7d)
│ $24.50 · 47 msg                resets in 6d 4h   │
├──────────────────────────────────────────────────┤
│ Today: 2.1M tok · 14 msg          Updated 14:32  │
└──────────────────────────────────────────────────┘  ← 300 px
```

**Key design choices:**

- The dominant figure on each row is **tokens-used** (e.g. `1.0M`, `5.4M`),
  formatted compactly (K / M suffixes). This is the "soul number" — the
  user's stated primary metric.
- The bar's fill is **time progress**, not quota. Bar fraction =
  `(now − started_at) / (resets_at − started_at)`, clamped to [0, 1]. This is
  factual — there is no fictional cap underlying it.
- The meta line carries supporting numbers — cost in USD, message count, and
  (for the 5 h block only) instantaneous burn rate in tokens-per-minute.
- Footer summarises today's local-day totals plus a freshness timestamp; goes
  STALE per §8 when the last push is > 5 min old.
- Monospace, heavy weight, high contrast. The panel is a 1 bpp mono reflective
  LCD (Sitronix ST7305) — solid black on off-white, no greys, no colors. Bars
  are solid-fill rectangles.
- All text sizes chosen so the two big token figures and both bars are legible
  from ~1.5 m.

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

1. **ccusage 5h-block coverage — confirmed adequate.** `ccusage blocks --active
   --json` exposes `startTime`, `endTime`, `totalTokens`, `costUSD`,
   `burnRate.tokensPerMinute`, and entry counts directly. See
   `docs/superpowers/notes/2026-04-26-ccusage-investigation.md`. **Closed** by
   Task 1 of the implementation plan.
2. **Weekly window — Anthropic's clock vs. our local approximation.** Anthropic's
   weekly limit "resets seven days after your session starts" (per their Max-plan
   support article), but Claude Code itself does **not** persist the weekly
   period start anywhere on disk (no local state file, no rate-limit headers in
   jsonl, `/status` is interactive-only). `push-usage.sh` therefore approximates
   `weekly.started_at` by scanning `~/.claude/projects/**/*.jsonl` for the
   earliest `type=="user"` event with `timestamp >= now − 168 h`.
   - **Single-machine accuracy:** matches Claude Code's `/status` clock
     to the second.
   - **Multi-machine drift:** if the user runs Claude Code on more than one
     machine and this Mac is missing the earliest session of the period, our
     `weekly.started_at` will be later than the truth, so `weekly.resets_at`
     will be displayed up to 7 days too late. **Confirmed: the user runs
     Claude Code on multiple machines.** v1 accepts the drift; weekly is
     best-effort. v2 candidate enhancements: (a) sync `~/.claude/projects/`
     across machines with iCloud/Syncthing, (b) have each Mac POST its local
     earliest-event timestamp to a shared store and reconcile.
   - See `docs/superpowers/notes/2026-04-26-claude-code-state-investigation.md`.
3. **LCD driver — no upstream LovyanGFX panel class.** The Waveshare ESP32-S3-
   RLCD-4.2 ships a Sitronix ST7305 mono reflective LCD with a non-linear 4×2
   tile framebuffer. LovyanGFX has no `Panel_ST7305`. Strategy: vendor
   Waveshare's `display_bsp.{h,cpp}` for chip init and use `LGFX_Sprite` plus
   `DisplayPort` to compose frames; blit on every dirty render. Closes the
   risk identified before Task 2; see
   `docs/superpowers/notes/2026-04-26-lcd-driver.md`.
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
