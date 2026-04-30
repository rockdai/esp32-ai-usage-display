# Claude Code Attention Alert — Design

**Date**: 2026-04-30
**Hardware**: Waveshare ESP32-S3-RLCD-4.2 (300×400 reflective LCD, no touch, KEY button)
**Scope**: v1.x — extend the existing usage display with a "Claude needs your attention"
overlay. Codex CLI still deferred.
**Supersedes**: nothing (additive). The 2026-04-26 spec stays canonical for the idle/usage
screen.

## 1. Purpose

The existing display answers *"how much have I burned?"*. It does not answer *"is Claude
waiting on me right now?"* — and that second question matters more, because if Claude is
done or asking for input you want to know **immediately**, even when the terminal window
is offscreen.

This feature adds a second, attention-grabbing screen that takes over the LCD whenever a
Claude Code session is producing events, and falls back to the existing usage screen
otherwise. The usage data is preserved in compact form on the new screen — not removed —
because seeing it shrink down on context switch is part of the visual signal.

> **Why a separate screen instead of an inline banner?** The user explicitly asked for a
> *full* layout reflow, not an inline notch. Two reasons it's the right call: (1) the
> reflective panel is small (400×300, 1 bpp) — splitting attention across "usage matters
> *and* attention matters" loses both. (2) The user's reaction-time requirement for
> WAITING is on the order of seconds; a banner sharing space with usage progress bars
> doesn't sell that urgency.

## 2. Goals & non-goals

### Goals

- Switch from the usage screen to an attention screen within ~1 second of a Claude Code
  hook firing on the user's Mac.
- Visually distinguish three Claude states — `WORKING`, `DONE`, `WAITING` — with a clear
  attention gradient (WORKING ≪ DONE ≪ WAITING).
- Preserve the user's existing 5h/weekly usage numbers on the attention screen, but
  shrunken to a single footer line.
- Return to the usage screen automatically (SessionEnd hook, or 15-min timeout) and
  dismissibly (KEY button).
- Survive Mac sleep, Wi-Fi flap, device reboot, and Claude Code crashes gracefully.

### Non-goals (v1.x)

- Codex CLI attention (deferred — see §13).
- Per-session tracking when the user runs >1 Claude terminal simultaneously. v1 takes
  the latest event and lets it overwrite (see §12 risk #2).
- Cross-machine reconciliation (each Mac that has the hooks installed pushes
  independently; the device shows whichever pushed last).
- Sound, haptics, RGB indicators (the device has none of these).
- A fourth "READY" state for `claude` startup → first prompt. SessionStart alone does
  not switch screens; the first `UserPromptSubmit` does (see §8.1).

## 3. Hard constraints (carried over)

These are the same constraints from the 2026-04-26 spec, restated because this feature
must respect them:

1. **No official AI provider APIs.** Detection signal must come from the user's local
   Claude Code installation — namely, Claude Code's hooks subsystem.
2. **No listening sockets on the Mac.** Hook scripts are short-lived shell scripts that
   `curl POST` outbound to the device. No daemon, no Unix socket bridge, no menu-bar app.
3. **No device-initiated network requests.** The ESP32 only listens. New endpoint
   `POST /attention` is added to the existing HTTP server; the device never polls.

## 4. Architecture

```
┌─────────────────────── Mac ───────────────────────────────┐
│ existing path (unchanged):                                │
│   launchd 60s → push-usage.sh → POST /data → UsageData    │
│                                                           │
│ new path:                                                 │
│   Claude Code  ─┐                                         │
│      hooks     ─┼─→ mac/hooks/<event>.sh                  │
│      events    ─┘     ├─ source secrets.env (HOST)        │
│                       └─ curl --max-time 2 -sf            │
│                            -X POST $HOST/attention        │
└───────────────────────────────────────────────────────────┘
                  │ HTTP/JSON, LAN, mDNS hostname
                  ▼
┌─────────────────────── ESP32-S3-RLCD-4.2 ─────────────────┐
│   ├─ POST /data       → UsageData       (existing)        │
│   ├─ POST /attention  → AttentionState  (new)             │
│   ├─ KEY button poll  → force AttentionKind=IDLE (new)    │
│   └─ render tick (1 Hz):                                  │
│        if attention.kind != IDLE: drawScreenB             │
│        else                     : drawScreenA  (existing) │
└───────────────────────────────────────────────────────────┘
```

The two data paths are independent. If the user has not installed the hooks, the
`/attention` endpoint is simply never hit — `attention.kind` stays `IDLE`, the device
never leaves Screen A. Existing functionality degrades to nothing.

## 5. Components

### 5.1 Mac side (new)

```
mac/
├── hooks/
│   ├── user-prompt-submit.sh    # POST {state:"WORKING"}
│   ├── stop.sh                  # POST {state:"DONE"}
│   ├── notification.sh          # POST {state:"WAITING"}
│   └── session-end.sh           # POST {state:"IDLE"}
├── install-hooks.sh             # writes ~/.claude/settings.json hooks block
└── uninstall-hooks.sh           # removes the hooks block, leaves rest of settings
```

**Hook script shape** (each script is ~10 lines):

```bash
#!/usr/bin/env bash
# mac/hooks/stop.sh — fires when Claude finishes a response.
set -u
DIR="$(cd "$(dirname "$0")/.." && pwd)"
. "$DIR/secrets.env"   # provides HOST=ai-desktop-buddy.local
TS="$(date +%s)"
CWD="${CLAUDE_PROJECT_DIR:-$PWD}"
SID="${CLAUDE_SESSION_ID:-}"
PAYLOAD="$(jq -nc \
  --argjson ts "$TS" --arg state DONE --arg cwd "$CWD" --arg sid "$SID" \
  '{ts:$ts, state:$state, cwd:$cwd, session_id:$sid}')"
curl --max-time 2 -sf -X POST -H 'Content-Type: application/json' \
     --data "$PAYLOAD" "http://${HOST}/attention" >/dev/null 2>&1 || true
```

Notes:
- `set -u` (no `-e`): we never want a failing curl to bubble up and block Claude. The
  `|| true` pins exit 0.
- 2-second curl timeout is the upper bound on how long Claude can stall on us.
- `CLAUDE_PROJECT_DIR` and `CLAUDE_SESSION_ID` are conventional env vars that Claude
  Code passes to hooks. We read with shell defaults so the script also works when run
  manually for testing.

**Installer** writes a JSON block to `~/.claude/settings.json` like:

```json
{
  "hooks": {
    "UserPromptSubmit": [{ "type": "command", "command": "/Users/rock/code/esp32-ai-usage-display/mac/hooks/user-prompt-submit.sh" }],
    "Stop":             [{ "type": "command", "command": "/Users/rock/code/esp32-ai-usage-display/mac/hooks/stop.sh" }],
    "Notification":     [{ "type": "command", "command": "/Users/rock/code/esp32-ai-usage-display/mac/hooks/notification.sh" }],
    "SessionEnd":       [{ "type": "command", "command": "/Users/rock/code/esp32-ai-usage-display/mac/hooks/session-end.sh" }]
  }
}
```

The installer must merge with any existing user `hooks` block (don't clobber). Use `jq`
to round-trip the file.

### 5.2 ESP32 firmware (changes)

**New files:**

```
firmware/src/attention.cpp / .h
   AttentionKind enum + AttentionState struct
   parseAttentionJson(const char* body, AttentionState& out)
   attentionTick(uint32_t now_ms): handles 15-min timeout, returns true if state changed

firmware/src/key.cpp / .h
   keyInit(): configures KEY GPIO with pullup
   keyPressed(): debounced edge-triggered "press happened since last call"
```

**Modified files:**

```
firmware/src/state.h
   + enum AttentionKind { IDLE=0, WORKING, DONE, WAITING };
   + struct AttentionState {
       AttentionKind kind = IDLE;
       uint32_t      since_ms = 0;     // millis() when current kind was entered
       char          cwd[64] = {0};
       // session_id intentionally not stored on device in v1
       bool          valid = false;
     };

firmware/src/main.cpp
   + server.on("/attention", HTTP_POST, handleAttention)
   + key tick in loop()
   + render dispatch: drawScreenA vs drawScreenB based on attention.kind

firmware/src/render.cpp / .h
   + drawScreenB(const AttentionState& a, const UsageData& u, uint32_t now_ms)
   + drawCompactUsageFooter(const UsageData& u)  -- the single-line bottom strip
   + drawStateBadge(AttentionKind, uint32_t since_ms)
   existing drawHeader / drawWindow / drawFooter unchanged
```

**KEY button — GPIO unconfirmed.** The Waveshare schematic lists three buttons
(BOOT / PWR / KEY). BOOT is GPIO0 (reserved for flashing). The pin number for the
user-labeled "KEY" specifically is to be pinned down in the implementation plan
(read pinout doc + probe sketch) before `key.cpp` is written. Once confirmed, the
software side is fixed: configure `INPUT_PULLUP`, active-low, 30 ms debounce.

## 6. Wire schema

### 6.1 `POST /attention`

Request body:

```json
{
  "ts":         1745673120,
  "state":      "WORKING",
  "cwd":        "/Users/rock/code/ai-desktop-buddy",
  "session_id": "01J7Z..."
}
```

| field      | type   | required | notes                                               |
|------------|--------|----------|-----------------------------------------------------|
| ts         | uint32 | yes      | epoch seconds; used for log correlation, not render |
| state      | string | yes      | one of `WORKING`, `DONE`, `WAITING`, `IDLE`         |
| cwd        | string | no       | full path; device truncates to basename for display |
| session_id | string | no       | reserved for v2 priority queue; v1 ignores          |

**Validation rules** (return 400 on any failure, leave `attention` state untouched):

- `ts` must be present and parseable as uint32.
- `state` must be one of the four literals (case-sensitive).
- `cwd`, if present, must be a string. Any length is accepted on the wire; on
  store, the device truncates with `strlcpy` into the 64-byte field (so practical
  ceiling is 63 visible chars).
- Total request body cap: 1 KB (server-level, ESP32 `WebServer` setting).
  Bodies over 1 KB are rejected by the framework before validation runs.

**Response**: `200 ok` on success, `400 <reason>` on validation failure. No body needed
on success.

### 6.2 `POST /data`

Unchanged. See 2026-04-26-esp32-ai-usage-display-design.md §6.

## 7. Layout (Screen B)

400 × 300 landscape, 1 bpp, identical canvas as Screen A.

```
┌──────────────────────────────────────────────────┐  y=0
│ CLAUDE  ai-desktop-buddy                         │  header strip y=0..43
├──────────────────────────────────────────────────┤  y=44 separator
│                                                  │
│                                                  │
│         ┌──────────────────────────┐             │  badge area y=80..180
│         │                          │             │  (state-styled — see 7.2)
│         │      W A I T I N G       │             │
│         │                          │             │
│         └──────────────────────────┘             │
│                                                  │
│              asking 12m                          │  duration line y=200, size 2
│                                                  │
│                                                  │
├──────────────────────────────────────────────────┤  y=270 separator
│ 5H 1.0M ▓▓▓▓▓░░░ 4h30m   Wk 5.4M ▓░░ 6d 4h       │  compact footer y=280, size 2
└──────────────────────────────────────────────────┘  y=300
```

### 7.1 Header (Screen B)

Same baseline as Screen A so the eye doesn't have to relocate when screens swap:
- "CLAUDE" size 4 left at y=8 (same x, same baseline as Screen A's "CLAUDE CODE",
  but trimmed to one word so the cwd basename has room).
- cwd basename size 2 right-aligned to x=392, baseline matched to "CLAUDE". If
  basename > 16 chars, truncate to `…tail`.
- WiFi indicator at top-right corner same as Screen A (§render.cpp drawHeader).
- Separator hline at y=44.

### 7.2 State badge (the visual delta between WORKING / DONE / WAITING)

All three render in the same y=80..180 region, centered horizontally. The visual
weight changes by font size + box decoration only — same drawing routine, three
parameter sets:

| state    | text      | size | box         | fill        | rationale                       |
|----------|-----------|------|-------------|-------------|---------------------------------|
| WORKING  | `WORKING` | 4    | none        | none        | low: no action needed yet       |
| DONE     | `DONE`    | 6    | 1 px border | none        | mid: come look when you can     |
| WAITING  | `WAITING` | 6    | 2 px border | reverse-video (filled black, white text) | high: switch back now |

Reverse-video on a 1 bpp panel is unambiguous: the WAITING badge is the only black
rectangle on the screen, visible across the room. The DONE outline is identical
geometry minus the fill, so the eye reads it as "the same thing, but quieter".

### 7.3 Duration line

`<verb> <Nm>` size 2, centered horizontally at y=200. Verbs:

| state    | line                |
|----------|---------------------|
| WORKING  | `working 3m`        |
| DONE     | `done 5m`           |
| WAITING  | `asking 12m`        |

Where `Nm` = `(millis() - since_ms) / 60000`. Tick recomputes each second so the
duration counts up smoothly between hook events.

If the line would underflow ("just now" — N=0), render `<verb> 0m`. Don't try to
get cute with "just now"; consistency matters more than copy nuance on a small panel.

### 7.4 Compact usage footer

Single-line strip at y=280, size 2:

```
5H 1.0M ▓▓▓▓▓░░░ 4h30m   Wk 5.4M ▓░░░ 6d 4h
```

Components per window: `<label>` `<tokens>` `<8-cell mini bar>` `<reset duration>`.
Same `formatTokens` and `fmtDuration` helpers as Screen A. The bar is 8 cells wide
of size-2 chars (so ~96 px); fill ratio is the time-progress same as Screen A.

If `UsageData.valid == false` (no `/data` push has landed yet) the footer
renders just `5H —— · Wk ——` so the strip's height is preserved. Layout doesn't
shift between "have usage" and "no usage yet".

## 8. State machine

### 8.1 Device-side state

```
AttentionState {
  AttentionKind kind;       // IDLE | WORKING | DONE | WAITING
  uint32_t      since_ms;   // millis() at entry into current kind
  char          cwd[64];
  bool          valid;
}
```

`kind == IDLE` ⇒ render Screen A. Any other value ⇒ render Screen B.

### 8.2 Transitions

| Trigger                                          | Effect                                  |
|--------------------------------------------------|-----------------------------------------|
| `POST /attention {state:"WORKING"}`              | kind=WORKING, since_ms=now, cwd=payload |
| `POST /attention {state:"DONE"}`                 | kind=DONE,    since_ms=now, cwd=payload |
| `POST /attention {state:"WAITING"}`              | kind=WAITING, since_ms=now, cwd=payload |
| `POST /attention {state:"IDLE"}` (SessionEnd)    | kind=IDLE,    since_ms=now, cwd=""      |
| KEY button pressed (debounced)                   | kind=IDLE,    since_ms=now, cwd=""      |
| `millis() - since_ms > 15 * 60 * 1000`           | kind=IDLE,    since_ms=now, cwd=""      |
| ESP32 reboot                                     | kind=IDLE (default-initialized)         |

Notes:
- "Latest event wins." If the user has two Claude terminals open, hooks from both
  push to the same endpoint and the most recent one defines what's shown. v1
  accepted limitation; see §12 risk #2.
- Each event always rewrites `since_ms` regardless of whether `kind` changed
  (e.g., WORKING → WORKING resets the duration to 0m, because something happened).
- The 15-minute timeout uses `millis()` natural wraparound: `(uint32_t)(now - since_ms)`
  is correct across the ~49-day rollover.

### 8.3 What does *not* trigger a transition

- `SessionStart` hook. Fires too noisily (also on `/clear`, `/compact`, `/resume`)
  and doesn't reflect actual activity. Installer does not register it.
- `PreToolUse`, `PostToolUse`, `SubagentStop`. These would inflate the "WORKING"
  duration counter with stale events but provide no information the user wants.
- Successful `POST /data`. Usage updates are independent of attention.

### 8.4 What about state during boot / first connection?

Default-initialized `AttentionState` has `kind=IDLE`. After Wi-Fi joins, the device
serves both endpoints. Until the *first* `/attention` POST lands, we render Screen A
(or the "Waiting for first sync…" placeholder if `/data` hasn't landed either).
This is the same first-boot UX as today; nothing new.

## 9. Data flow (one cycle)

| Time           | Event                                                            |
|----------------|------------------------------------------------------------------|
| T+0            | User submits a prompt to Claude Code.                            |
| T+~10 ms       | `UserPromptSubmit` hook fires; bash script computes payload.     |
| T+~50 ms       | `curl POST /attention` hits ESP32; handler updates AttentionState. |
| T+~1 s         | Render tick redraws Screen B (WORKING) on next 1 Hz wakeup.      |
| ...            | User waits while Claude works.                                   |
| T+30 s         | Claude finishes; `Stop` hook fires; transition to DONE.          |
| T+30..15 min   | Device shows DONE with duration counting up. User glances.       |
| T+~15 min      | Either: user types again (→ WORKING), or types `/exit` (→ IDLE), |
|                | or hits KEY (→ IDLE), or 15 min timeout (→ IDLE).                |

Render is gated by `g_dirty` (set on POST) plus the 1 Hz tick (so the duration
counter and the timeout/transition fire even with no incoming events).

## 10. Error handling

| Scenario                                       | Behavior                                                  |
|------------------------------------------------|-----------------------------------------------------------|
| Mac → ESP32 network unreachable                | Hook curl fails inside the 2 s timeout; script exits 0; Claude not blocked. State on device unchanged. |
| Hook script not installed / Claude unaware     | No-op. `/attention` endpoint never hit. Device stays on Screen A. |
| Malformed `/attention` body                    | Server returns 400 with reason. AttentionState unchanged. |
| ESP32 reboot mid-session                       | AttentionState resets to IDLE; UsageData also empty until next 60 s push. Next hook event will restore Screen B. |
| Mac sleep with Claude still open               | Hooks don't fire (no Claude activity); device times out to IDLE after 15 min; Screen A may also go STALE per existing behavior. |
| Two hooks fire within ms of each other         | HTTP server is single-threaded; later one wins; no race. |
| KEY pressed when already IDLE                  | No-op (idempotent).                                       |
| KEY mechanical bounce                          | 30 ms debounce in `key.cpp`.                              |
| `cwd` payload very long (deeply nested path)   | Truncated to 63 chars on store (§11.2). Total body > 1 KB rejected by framework. |
| `millis()` rollover at ~49 days uptime         | `(uint32_t)(now - since_ms)` arithmetic remains correct.  |
| ccusage / `/data` push fails                   | Independent of attention. Footer on Screen B falls back to `——` placeholders. |

## 11. Testing & verification

### 11.1 Mac hook scripts

`mac/test/test-hooks.bats`:
- Each script invoked with `CLAUDE_PROJECT_DIR=/tmp/foo CLAUDE_SESSION_ID=abc` env.
- Stub `curl` (placed first on `PATH`) to capture argv → file. Assert:
  - URL is `http://${HOST}/attention`
  - Body parses as JSON, has correct `state`, contains the env-supplied cwd/sid.
  - Exit code is 0 even when stubbed curl returns non-zero (`|| true` semantics).
- Smoke: `install-hooks.sh` against a tempdir-pretending-to-be-`~/.claude` writes
  the expected JSON; `uninstall-hooks.sh` cleanly reverses it; both are idempotent.

### 11.2 Firmware native tests (`pio test -e native`)

`firmware/test/test_attention/test_attention.cpp` covers:
- `parseAttentionJson` valid inputs for each of the four states.
- Missing `ts` → false.
- Missing `state` → false.
- Unknown `state` literal → false.
- Missing `cwd` → true, cwd left empty.
- Oversize `cwd` (>63 chars) → truncate to 63, return true. (Hard rule per §6.1 —
  truncation is preferred over rejection so a deeply-nested project path doesn't
  silently break the alert.)
- `attentionTick`: simulates millis() advancement; asserts 15 min boundary triggers
  IDLE transition; before that, kind unchanged.

`firmware/test/test_render_screenb/` is **out of scope** for the native test suite —
LovyanGFX `LGFX_Sprite` doesn't link cleanly under the `native` env (depends on
Arduino types). Visual verification is by hardware photo (§11.3) instead.

### 11.3 End-to-end (real hardware)

A single hand-driven walk-through, recorded with photos:
1. Cold boot device. Verify Screen A appears after first `/data` push.
2. Open Claude in a terminal, submit prompt. Verify Screen B WORKING within 2 s,
   duration ticking each second.
3. Wait for Claude's response. Verify transition to DONE; duration resets to 0m.
4. Type a follow-up prompt. Verify back to WORKING.
5. Trigger a permission prompt (e.g., a Bash command). Verify WAITING; verify the
   reverse-video visual is distinguishable from DONE across the room (~1.5 m).
6. Press KEY. Verify immediate IDLE → Screen A.
7. Re-trigger DONE; leave it 16 minutes; verify auto-IDLE timeout fires.
8. `/exit` Claude. Verify SessionEnd → Screen A.

### 11.4 Out of scope

- Pixel-level snapshot tests of Screen B (the panel itself is the only ground truth).
- Multi-Mac concurrent testing (v1 accepts last-event-wins, see §12).
- Hook installer schema migration when Claude Code changes the settings.json format
  (we'll fix it when it breaks).

## 12. Risks & open questions

1. **Hook latency under heavy Claude load.** Hooks run synchronously in the same
   process as Claude Code. A blocked `curl` would stall Claude for up to its
   timeout. Mitigation: `--max-time 2` on every curl; `set -u` (no `-e`) plus
   `|| true` to never propagate failure; payload size capped at well under 1 KB.
   *Closed by design;* will spot-check in §11.3 step 2 latency.

2. **Multi-session conflicts on one Mac.** If the user has two `claude` terminals
   open, both fire hooks at the same endpoint. The device shows the latest event;
   so terminal A's `WAITING` can be overwritten by terminal B's incidental `Stop`.
   v1 accepts. v2 candidate enhancement: keep a small map keyed by `session_id`
   and prioritize `WAITING > DONE > WORKING`; expire entries on `SessionEnd` or
   timeout. The wire schema already includes `session_id` for this v2 work.

3. **Multi-machine drift.** Each Mac with hooks installed pushes independently.
   Device shows the latest event from any machine, with no notion of "which Mac".
   Same shape of risk as the existing weekly-window drift (2026-04-26 spec §11.2);
   v1 accepts.

4. **KEY GPIO unconfirmed.** Schematic lists three buttons but the GPIO
   number for the user-labeled "KEY" needs verification from the Waveshare
   pinout doc (or a probe sketch) before `key.cpp` is written. Implementation
   plan must resolve this before Task K (KEY handling).

5. **Claude Code hooks API stability.** Hooks are part of Claude Code's published
   feature set, but the env var names (`CLAUDE_PROJECT_DIR`, `CLAUDE_SESSION_ID`)
   could change across CLI versions. Mitigation: scripts use shell defaults so a
   missing env var degrades to "no cwd / no session_id" rather than failing.

6. **15-minute timeout interaction with active sessions.** If the user is reading
   Claude's long output for >15 min without interacting, device drops to IDLE while
   they're still "in" the session. Acceptable: when they reply, `UserPromptSubmit`
   fires and Screen B comes back. The displayed `done 16m` going to IDLE is no
   worse than going to `done 16m` going to `done 17m`.

## 13. Future (deferred, not in v1.x)

- Codex CLI attention. Codex doesn't have a hooks subsystem comparable to Claude
  Code's; would require process monitoring or shell-prompt-trampoline tricks. Out
  of scope until the user actually uses Codex enough for it to matter.
- v2 multi-session priority queue (see §12.2).
- Sound/haptic alert. The device has no buzzer; would need hardware mod.
- Battery-aware behavior. Same deferral as the 2026-04-26 spec §12.
- Statistical view: "DONE 23 times today" / "longest WAITING 12 minutes". Could
  live alongside the today line on Screen A. Out of scope for v1.x.

## 14. v1.0 implementation status (post-e2e)

Two deviations from this design were absorbed during the end-to-end walkthrough.
Captured here so a reader of this spec alone is not misled. Full context lives
in `docs/superpowers/notes/2026-04-30-attention-e2e-log.md`.

1. **KEY-dismiss path removed.** The user's actual board variant lacks the third
   physical button (only BOOT is present); the GPIO-18 KEY described in §5.2
   and §8.2 was never reachable. `key.h`/`key.cpp` were deleted and the wires
   in `main.cpp` removed. Clearance for v1.0 is via SessionEnd hook + 15-min
   timeout only. §8.2's "KEY pressed" transition row is therefore aspirational;
   §12 risk #4 is closed in the negative (no KEY at all, not just unconfirmed
   GPIO).

2. **Compact-usage footer simplified.** The §7.4 example string
   `5H 1.0M ▓▓▓▓▓░░░ 4h30m  Wk 5.4M ▓░░ 6d 4h` overflowed 400 px when weekly
   tokens reached 9-figure territory; the implementation drops the mini-bars
   and uses integer-M token format. The current footer reads
   `5H <int>M <reset>  Wk <int>M <reset>` (e.g. `5H 66M 4h 32m  Wk 497M 6d 1h`)
   and uses `setTextWrap(false)` defensively so future overflow clips at the
   right edge instead of wrapping below.

3. **15-minute timeout** is unit-tested (`test_tick_past_timeout_goes_idle` +
   `test_tick_handles_millis_rollover`) but was not live-verified end-to-end.
   This is acceptable for v1.0 — the logic is pure arithmetic, the tests cover
   the boundary and rollover paths, and the cost of a 16-minute live test in
   real time was not worth blocking shipment.
