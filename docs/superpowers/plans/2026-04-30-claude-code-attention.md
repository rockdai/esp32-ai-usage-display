# Claude Code Attention Alert — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a second LCD screen that takes over when Claude Code is producing events (WORKING/DONE/WAITING), driven by Claude Code hooks pushing to a new `POST /attention` endpoint. Idle returns to the existing usage screen via SessionEnd hook, 15-min timeout, or KEY button.

**Architecture:** Mac-side hook scripts (one per relevant Claude event) `curl POST` to a new firmware endpoint. Firmware tracks an `AttentionState` independently of `UsageData`; render dispatcher chooses Screen A (existing) vs Screen B (new) based on `attention.kind != IDLE`.

**Tech Stack:** bash + jq + curl on Mac; PlatformIO/Arduino + LovyanGFX + ArduinoJson on ESP32-S3-RLCD-4.2; bats for Mac tests; Unity (PlatformIO native) for firmware tests.

**Spec:** `docs/superpowers/specs/2026-04-30-claude-code-attention-design.md`

---

## File map

**Created**
```
mac/hooks/_lib.sh                         shared post_attention() helper
mac/hooks/user-prompt-submit.sh           1-liner → post_attention WORKING
mac/hooks/stop.sh                         1-liner → post_attention DONE
mac/hooks/notification.sh                 1-liner → post_attention WAITING
mac/hooks/session-end.sh                  1-liner → post_attention IDLE
mac/install-hooks.sh                      idempotent jq merge into ~/.claude/settings.json
mac/uninstall-hooks.sh                    reverse of install
mac/test/test-hooks.bats                  bats tests for _lib.sh + install/uninstall
firmware/src/attention.h                  parser + tick interface
firmware/src/attention.cpp                impl
firmware/src/key.h                        debounced KEY interface
firmware/src/key.cpp                      impl
firmware/test/test_attention/test_attention.cpp   native tests
docs/superpowers/notes/2026-04-30-key-gpio-probe.md   KEY pin verification
```

**Modified**
```
firmware/src/state.h          + AttentionKind enum + AttentionState struct
firmware/src/render.h         + AttentionState arg to renderTick
firmware/src/render.cpp       + drawScreenB + drawStateBadge + drawCompactUsageFooter
                              + dispatch in renderTick
firmware/src/main.cpp         + g_attention + handleAttention + KEY tick + render call
firmware/platformio.ini       + attention.cpp to native env build_src_filter
```

---

## Task 1: Mac hook helper `_lib.sh` + bats tests

**Files:**
- Create: `mac/hooks/_lib.sh`
- Create: `mac/test/test-hooks.bats`

The shared helper centralizes payload construction and curl invocation. Each per-event hook will be a 2-line wrapper around `post_attention <STATE>`.

- [ ] **Step 1.1: Write the failing bats test for `post_attention`**

Create `mac/test/test-hooks.bats`:

```bash
#!/usr/bin/env bats
# Verifies _lib.sh post_attention builds the right payload and calls curl.

setup() {
  STAGE="$BATS_TEST_TMPDIR"
  mkdir -p "$STAGE/hooks" "$STAGE/stub"
  cp "$BATS_TEST_DIRNAME/../hooks/_lib.sh" "$STAGE/hooks/_lib.sh"
  echo "HOST=test.local" > "$STAGE/secrets.env"

  # Stub curl: capture argv and stdin to files; exit 0.
  cat > "$STAGE/stub/curl" <<STUB
#!/bin/sh
printf '%s\n' "\$@" > "$STAGE/curl-argv"
cat > "$STAGE/curl-body"
exit 0
STUB
  chmod +x "$STAGE/stub/curl"
  export PATH="$STAGE/stub:$PATH"
  export STAGE
}

@test "post_attention DONE posts to /attention with state=DONE" {
  CLAUDE_PROJECT_DIR=/tmp/proj CLAUDE_SESSION_ID=sid-1 \
    bash -c "source $STAGE/hooks/_lib.sh; post_attention DONE"

  run cat "$STAGE/curl-argv"
  echo "$output" | grep -q 'http://test.local/attention'
  echo "$output" | grep -q -- '--max-time 2'

  body="$(cat "$STAGE/curl-body")"
  [ "$(echo "$body" | jq -r .state)"      = "DONE" ]
  [ "$(echo "$body" | jq -r .cwd)"        = "/tmp/proj" ]
  [ "$(echo "$body" | jq -r .session_id)" = "sid-1" ]
}

@test "post_attention WAITING sets state=WAITING" {
  CLAUDE_PROJECT_DIR=/tmp/proj CLAUDE_SESSION_ID=sid-1 \
    bash -c "source $STAGE/hooks/_lib.sh; post_attention WAITING"

  body="$(cat "$STAGE/curl-body")"
  [ "$(echo "$body" | jq -r .state)" = "WAITING" ]
}

@test "post_attention with no \$CLAUDE_PROJECT_DIR falls back to \$PWD" {
  unset CLAUDE_PROJECT_DIR CLAUDE_SESSION_ID
  ( cd /tmp && bash -c "source $STAGE/hooks/_lib.sh; post_attention WORKING" )

  body="$(cat "$STAGE/curl-body")"
  [ "$(echo "$body" | jq -r .cwd)" = "/tmp" ]
  [ "$(echo "$body" | jq -r .session_id)" = "" ]
}

@test "post_attention exits 0 when curl fails (does not block Claude)" {
  cat > "$STAGE/stub/curl" <<'STUB'
#!/bin/sh
exit 28
STUB
  chmod +x "$STAGE/stub/curl"

  run bash -c "source $STAGE/hooks/_lib.sh; post_attention DONE"
  [ "$status" -eq 0 ]
}
```

- [ ] **Step 1.2: Run the test to verify it fails**

```bash
cd mac && bats test/test-hooks.bats
```

Expected: 4 tests fail with `cp: ...hooks/_lib.sh: No such file or directory` (or similar — `_lib.sh` doesn't exist yet).

- [ ] **Step 1.3: Implement `_lib.sh`**

Create `mac/hooks/_lib.sh`:

```bash
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

  curl --max-time 2 -sf \
       -X POST -H 'Content-Type: application/json' \
       --data "$payload" \
       "http://${HOST}/attention" \
       >/dev/null 2>&1 || true
}
```

- [ ] **Step 1.4: Run the test to verify it passes**

```bash
cd mac && bats test/test-hooks.bats
```

Expected: all 4 tests pass.

- [ ] **Step 1.5: Commit**

```bash
git add mac/hooks/_lib.sh mac/test/test-hooks.bats
git commit -m "mac: hooks/_lib.sh post_attention helper + bats tests"
```

---

## Task 2: Four per-event hook scripts

**Files:**
- Create: `mac/hooks/user-prompt-submit.sh`
- Create: `mac/hooks/stop.sh`
- Create: `mac/hooks/notification.sh`
- Create: `mac/hooks/session-end.sh`

These are 1-line wrappers. No new tests — `_lib.sh` is already covered by Task 1, and the wrappers contain no logic worth re-testing.

- [ ] **Step 2.1: Create `mac/hooks/user-prompt-submit.sh`**

```bash
#!/usr/bin/env bash
# Fires on Claude Code's UserPromptSubmit hook → state WORKING.
. "$(dirname "$0")/_lib.sh"
post_attention WORKING
```

- [ ] **Step 2.2: Create `mac/hooks/stop.sh`**

```bash
#!/usr/bin/env bash
# Fires on Claude Code's Stop hook (response complete) → state DONE.
. "$(dirname "$0")/_lib.sh"
post_attention DONE
```

- [ ] **Step 2.3: Create `mac/hooks/notification.sh`**

```bash
#!/usr/bin/env bash
# Fires on Claude Code's Notification hook (needs user input) → state WAITING.
. "$(dirname "$0")/_lib.sh"
post_attention WAITING
```

- [ ] **Step 2.4: Create `mac/hooks/session-end.sh`**

```bash
#!/usr/bin/env bash
# Fires on Claude Code's SessionEnd hook (session closing) → state IDLE.
. "$(dirname "$0")/_lib.sh"
post_attention IDLE
```

- [ ] **Step 2.5: Make all four executable**

```bash
chmod +x mac/hooks/{user-prompt-submit,stop,notification,session-end}.sh
```

- [ ] **Step 2.6: Smoke-test one hook end-to-end (with stubbed curl)**

```bash
cd "$(git rev-parse --show-toplevel)/mac"
TMP="$(mktemp -d)"
cat > "$TMP/curl" <<'STUB'
#!/bin/sh
echo "URL=$*"
cat
STUB
chmod +x "$TMP/curl"
echo "HOST=test.local" > /tmp/secrets-stub.env

# point hook at our stub secrets and stub curl
PATH="$TMP:$PATH" CLAUDE_PROJECT_DIR=/tmp CLAUDE_SESSION_ID=demo \
  bash -c '. ./hooks/_lib.sh; HOST=test.local; post_attention DONE' </dev/null
```

Expected: curl prints `URL=` line containing `http://test.local/attention` followed by a JSON body with `"state":"DONE"`.

- [ ] **Step 2.7: Commit**

```bash
git add mac/hooks/user-prompt-submit.sh mac/hooks/stop.sh \
        mac/hooks/notification.sh mac/hooks/session-end.sh
git commit -m "mac: 4 Claude Code hook scripts (UserPromptSubmit/Stop/Notification/SessionEnd)"
```

---

## Task 3: `install-hooks.sh` + `uninstall-hooks.sh` + bats tests

**Files:**
- Create: `mac/install-hooks.sh`
- Create: `mac/uninstall-hooks.sh`
- Modify: `mac/test/test-hooks.bats` (append install/uninstall tests)

Idempotent merge into `~/.claude/settings.json` using `jq`. Both scripts take an optional `--settings <path>` for testing against a temp file instead of `~/.claude/settings.json`.

- [ ] **Step 3.1: Append failing bats tests for install/uninstall**

Append to `mac/test/test-hooks.bats`:

```bash
@test "install-hooks creates hooks block when settings.json is empty" {
  echo '{}' > "$STAGE/settings.json"
  bash "$BATS_TEST_DIRNAME/../install-hooks.sh" --settings "$STAGE/settings.json"

  v="$(jq -r '.hooks.Stop[0].command' "$STAGE/settings.json")"
  echo "$v" | grep -q 'mac/hooks/stop.sh$'

  v="$(jq -r '.hooks.UserPromptSubmit[0].command' "$STAGE/settings.json")"
  echo "$v" | grep -q 'mac/hooks/user-prompt-submit.sh$'

  v="$(jq -r '.hooks.Notification[0].command' "$STAGE/settings.json")"
  echo "$v" | grep -q 'mac/hooks/notification.sh$'

  v="$(jq -r '.hooks.SessionEnd[0].command' "$STAGE/settings.json")"
  echo "$v" | grep -q 'mac/hooks/session-end.sh$'
}

@test "install-hooks preserves unrelated keys" {
  cat > "$STAGE/settings.json" <<'JSON'
{ "model": "opus", "theme": "dark" }
JSON
  bash "$BATS_TEST_DIRNAME/../install-hooks.sh" --settings "$STAGE/settings.json"

  [ "$(jq -r .model "$STAGE/settings.json")" = "opus" ]
  [ "$(jq -r .theme "$STAGE/settings.json")" = "dark" ]
  [ "$(jq -r '.hooks.Stop[0].type' "$STAGE/settings.json")" = "command" ]
}

@test "install-hooks is idempotent (re-run produces same output)" {
  echo '{}' > "$STAGE/settings.json"
  bash "$BATS_TEST_DIRNAME/../install-hooks.sh" --settings "$STAGE/settings.json"
  cp "$STAGE/settings.json" "$STAGE/after-1.json"
  bash "$BATS_TEST_DIRNAME/../install-hooks.sh" --settings "$STAGE/settings.json"
  diff "$STAGE/after-1.json" "$STAGE/settings.json"
}

@test "install-hooks merges with pre-existing user hooks (does not duplicate or clobber)" {
  cat > "$STAGE/settings.json" <<'JSON'
{ "hooks": { "Stop": [{ "type": "command", "command": "/usr/bin/true" }] } }
JSON
  bash "$BATS_TEST_DIRNAME/../install-hooks.sh" --settings "$STAGE/settings.json"

  # Pre-existing entry must still be present
  count="$(jq '.hooks.Stop | length' "$STAGE/settings.json")"
  [ "$count" = "2" ]
  [ "$(jq -r '.hooks.Stop[0].command' "$STAGE/settings.json")" = "/usr/bin/true" ]
}

@test "uninstall-hooks removes our entries, preserves user entries" {
  bash "$BATS_TEST_DIRNAME/../install-hooks.sh" --settings "$STAGE/settings.json" 2>/dev/null || true
  echo '{ "hooks": { "Stop": [{ "type": "command", "command": "/usr/bin/true" }] } }' > "$STAGE/settings.json"
  bash "$BATS_TEST_DIRNAME/../install-hooks.sh"   --settings "$STAGE/settings.json"
  bash "$BATS_TEST_DIRNAME/../uninstall-hooks.sh" --settings "$STAGE/settings.json"

  # Only the user's pre-existing entry should remain
  [ "$(jq '.hooks.Stop | length' "$STAGE/settings.json")" = "1" ]
  [ "$(jq -r '.hooks.Stop[0].command' "$STAGE/settings.json")" = "/usr/bin/true" ]

  # Our hooks for events with no other entries should be empty arrays or removed
  none_left="$(jq -r '.hooks.Notification // [] | map(select(.command|contains("ai-desktop-buddy"))) | length' "$STAGE/settings.json")"
  [ "$none_left" = "0" ]
}
```

- [ ] **Step 3.2: Run tests, expect failure**

```bash
cd mac && bats test/test-hooks.bats
```

Expected: 5 install/uninstall tests fail (scripts don't exist yet); the original 4 still pass.

- [ ] **Step 3.3: Implement `install-hooks.sh`**

Create `mac/install-hooks.sh`:

```bash
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
```

- [ ] **Step 3.4: Implement `uninstall-hooks.sh`**

Create `mac/uninstall-hooks.sh`:

```bash
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
```

- [ ] **Step 3.5: Make both executable**

```bash
chmod +x mac/install-hooks.sh mac/uninstall-hooks.sh
```

- [ ] **Step 3.6: Run tests, expect all to pass**

```bash
cd mac && bats test/test-hooks.bats
```

Expected: all 9 tests pass (4 from Task 1 + 5 new).

> **Note on "with_entries"**: jq's `with_entries(...|select(...))` form is a known pattern but the `| select(.value | length > 0)` filter needs to wrap the *transformed* entry, so the construct above expressed as `.value |= map(...)` then `select(.value|length>0)` runs both operations on the same key/value pair. If the test for "remove empty event keys" fails, fall back to a two-pass: first map to filter values, then `with_entries(select(.value|length > 0))`.

- [ ] **Step 3.7: Commit**

```bash
git add mac/install-hooks.sh mac/uninstall-hooks.sh mac/test/test-hooks.bats
git commit -m "mac: install/uninstall scripts that merge hooks into settings.json"
```

---

## Task 4: `state.h` — `AttentionKind` + `AttentionState`

**Files:**
- Modify: `firmware/src/state.h`

Pure data definitions. No standalone test (used by all subsequent tasks; Task 5 catches any compile breakage).

- [ ] **Step 4.1: Append to `firmware/src/state.h`**

Add after the existing `UsageData` struct, before the closing of the file:

```cpp
// ---- Attention (Claude Code activity) -----------------------------------

enum AttentionKind : uint8_t {
  ATTN_IDLE    = 0,   // no active session — render Screen A (usage)
  ATTN_WORKING = 1,   // user submitted prompt; Claude processing
  ATTN_DONE    = 2,   // Claude finished; user should look
  ATTN_WAITING = 3,   // Claude needs input (permission prompt etc)
};

struct AttentionState {
  AttentionKind kind     = ATTN_IDLE;
  uint32_t      since_ms = 0;     // millis() at entry into current kind
  char          cwd[64]  = {0};   // truncated to 63 chars + null
  bool          valid    = false; // false until first event lands
};
```

> Naming note: the enum members are prefixed `ATTN_` to avoid `IDLE`/`WORKING`/etc. colliding with anything else in the global namespace. The wire schema's `state` literals are still uppercase `WORKING`/`DONE`/etc — the parser maps strings to enum values.

- [ ] **Step 4.2: Sanity-build**

```bash
cd firmware && pio run -e rlcd42
```

Expected: build succeeds. (No callers yet, so nothing references the new types — but state.h must still compile.)

- [ ] **Step 4.3: Commit**

```bash
git add firmware/src/state.h
git commit -m "firmware: AttentionKind enum + AttentionState struct in state.h"
```

---

## Task 5: `parseAttentionJson` + native tests

**Files:**
- Create: `firmware/src/attention.h`
- Create: `firmware/src/attention.cpp`
- Create: `firmware/test/test_attention/test_attention.cpp`
- Modify: `firmware/platformio.ini` (add `attention.cpp` to native build)

- [ ] **Step 5.1: Write `firmware/src/attention.h`** (interface only — no impl yet so the test fails on link)

```cpp
#pragma once
#include "state.h"

// Parse the body of POST /attention. Returns true and populates `out` on
// success; false on any validation failure. On false return, `out` is in
// an indeterminate state (caller must not consume it).
//
// Required fields: "ts" (uint32), "state" (one of "WORKING" | "DONE"
// | "WAITING" | "IDLE"; case-sensitive).
//
// Optional fields: "cwd" (string; truncated to 63 chars on store),
// "session_id" (ignored in v1).
//
// `out.since_ms` is NOT set here — the HTTP handler stamps it with the
// device's current millis() *after* a successful parse.
bool parseAttentionJson(const char* body, AttentionState& out);

// Returns true if the timeout caused a transition to ATTN_IDLE.
// Called every render tick. Uses `(uint32_t)(now_ms - s.since_ms)` so
// millis() rollover is handled without conditionals.
bool attentionTick(AttentionState& s, uint32_t now_ms);
```

- [ ] **Step 5.2: Write the failing parser tests**

Create `firmware/test/test_attention/test_attention.cpp`:

```cpp
#include <unity.h>
#include <string.h>
#include "attention.h"

void setUp(void) {}
void tearDown(void) {}

// ---- parseAttentionJson --------------------------------------------------

void test_parse_valid_working() {
  const char* body = R"({
    "ts": 1745673120, "state": "WORKING",
    "cwd": "/Users/rock/code/foo", "session_id": "abc-123"
  })";
  AttentionState a;
  TEST_ASSERT_TRUE(parseAttentionJson(body, a));
  TEST_ASSERT_EQUAL(ATTN_WORKING, a.kind);
  TEST_ASSERT_EQUAL_STRING("/Users/rock/code/foo", a.cwd);
  TEST_ASSERT_TRUE(a.valid);
}

void test_parse_done_waiting_idle() {
  AttentionState a;
  TEST_ASSERT_TRUE(parseAttentionJson(R"({"ts":1,"state":"DONE"})", a));
  TEST_ASSERT_EQUAL(ATTN_DONE, a.kind);
  TEST_ASSERT_TRUE(parseAttentionJson(R"({"ts":1,"state":"WAITING"})", a));
  TEST_ASSERT_EQUAL(ATTN_WAITING, a.kind);
  TEST_ASSERT_TRUE(parseAttentionJson(R"({"ts":1,"state":"IDLE"})", a));
  TEST_ASSERT_EQUAL(ATTN_IDLE, a.kind);
}

void test_parse_missing_ts_rejected() {
  AttentionState a;
  TEST_ASSERT_FALSE(parseAttentionJson(R"({"state":"WORKING"})", a));
}

void test_parse_missing_state_rejected() {
  AttentionState a;
  TEST_ASSERT_FALSE(parseAttentionJson(R"({"ts":1})", a));
}

void test_parse_unknown_state_rejected() {
  AttentionState a;
  TEST_ASSERT_FALSE(parseAttentionJson(R"({"ts":1,"state":"BOGUS"})", a));
}

void test_parse_lowercase_state_rejected() {
  AttentionState a;
  TEST_ASSERT_FALSE(parseAttentionJson(R"({"ts":1,"state":"working"})", a));
}

void test_parse_missing_cwd_ok() {
  AttentionState a;
  TEST_ASSERT_TRUE(parseAttentionJson(R"({"ts":1,"state":"WORKING"})", a));
  TEST_ASSERT_EQUAL(ATTN_WORKING, a.kind);
  TEST_ASSERT_EQUAL_STRING("", a.cwd);
}

void test_parse_long_cwd_truncated() {
  // 80 'a' characters in cwd; should truncate to 63 + null.
  const char* body = R"({"ts":1,"state":"WORKING","cwd":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"})";
  AttentionState a;
  TEST_ASSERT_TRUE(parseAttentionJson(body, a));
  TEST_ASSERT_EQUAL_INT(63, (int)strlen(a.cwd));
  for (int i = 0; i < 63; ++i) TEST_ASSERT_EQUAL_CHAR('a', a.cwd[i]);
}

// ---- attentionTick --------------------------------------------------------

void test_tick_within_timeout_no_change() {
  AttentionState s;
  s.kind = ATTN_WORKING;
  s.since_ms = 1000;
  TEST_ASSERT_FALSE(attentionTick(s, 1000 + 14UL*60*1000));
  TEST_ASSERT_EQUAL(ATTN_WORKING, s.kind);
}

void test_tick_past_timeout_goes_idle() {
  AttentionState s;
  s.kind = ATTN_DONE;
  s.since_ms = 1000;
  TEST_ASSERT_TRUE(attentionTick(s, 1000 + 16UL*60*1000));
  TEST_ASSERT_EQUAL(ATTN_IDLE, s.kind);
}

void test_tick_idle_never_transitions() {
  AttentionState s;  // default ATTN_IDLE
  s.since_ms = 1000;
  TEST_ASSERT_FALSE(attentionTick(s, 1000 + 999UL*60*1000));
  TEST_ASSERT_EQUAL(ATTN_IDLE, s.kind);
}

void test_tick_handles_millis_rollover() {
  // since_ms close to uint32 max; now_ms wrapped past zero by ~2 minutes.
  // (uint32_t)(now - since) = (uint32_t)(0x00010000 - 0xFFFF0000) = 0x00020000
  // = 131072 ms ≈ 2 min. Still under the 15 min threshold → no change.
  AttentionState s;
  s.kind = ATTN_WAITING;
  s.since_ms = 0xFFFF0000UL;
  TEST_ASSERT_FALSE(attentionTick(s, 0x00010000UL));
  TEST_ASSERT_EQUAL(ATTN_WAITING, s.kind);
}

int runUnityTests(void) {
  UNITY_BEGIN();
  RUN_TEST(test_parse_valid_working);
  RUN_TEST(test_parse_done_waiting_idle);
  RUN_TEST(test_parse_missing_ts_rejected);
  RUN_TEST(test_parse_missing_state_rejected);
  RUN_TEST(test_parse_unknown_state_rejected);
  RUN_TEST(test_parse_lowercase_state_rejected);
  RUN_TEST(test_parse_missing_cwd_ok);
  RUN_TEST(test_parse_long_cwd_truncated);
  RUN_TEST(test_tick_within_timeout_no_change);
  RUN_TEST(test_tick_past_timeout_goes_idle);
  RUN_TEST(test_tick_idle_never_transitions);
  RUN_TEST(test_tick_handles_millis_rollover);
  return UNITY_END();
}

int main(int, char**) { return runUnityTests(); }
```

- [ ] **Step 5.3: Update `firmware/platformio.ini` native env**

Edit `firmware/platformio.ini`, change `build_src_filter`:

```ini
[env:native]
platform = native
test_framework = unity
test_build_src = yes
build_src_filter = +<api.cpp> +<attention.cpp>
build_flags = -std=gnu++17 -I src
lib_deps =
  bblanchon/ArduinoJson@^7.1.0
```

- [ ] **Step 5.4: Run native tests, expect failure**

```bash
cd firmware && pio test -e native -f test_attention
```

Expected: linker error (unresolved `parseAttentionJson` and `attentionTick`) — or, if you stub minimal bodies first, test failures.

- [ ] **Step 5.5: Implement `attention.cpp`**

Create `firmware/src/attention.cpp`:

```cpp
#include "attention.h"
#include <ArduinoJson.h>
#include <string.h>

namespace {

constexpr uint32_t kTimeoutMs = 15UL * 60UL * 1000UL;

// Maps wire literal to enum. Returns true on match.
bool parseStateLiteral(const char* s, AttentionKind& out) {
  if (!s) return false;
  if (strcmp(s, "WORKING") == 0) { out = ATTN_WORKING; return true; }
  if (strcmp(s, "DONE")    == 0) { out = ATTN_DONE;    return true; }
  if (strcmp(s, "WAITING") == 0) { out = ATTN_WAITING; return true; }
  if (strcmp(s, "IDLE")    == 0) { out = ATTN_IDLE;    return true; }
  return false;
}

// Truncating copy: at most dst_size-1 chars from src, always null-terminated.
// Mirrors strlcpy semantics so the platformio native env (no BSD strlcpy)
// builds without extra deps.
void copy_truncated(char* dst, size_t dst_size, const char* src) {
  if (dst_size == 0) return;
  if (!src) { dst[0] = '\0'; return; }
  size_t n = strlen(src);
  if (n >= dst_size) n = dst_size - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

}  // namespace

bool parseAttentionJson(const char* body, AttentionState& out) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  if (!doc["ts"].is<uint32_t>())   return false;

  const char* state_str = doc["state"] | (const char*)nullptr;
  AttentionKind kind;
  if (!parseStateLiteral(state_str, kind)) return false;

  // Reset to defaults so leftover state from previous parse can't leak.
  out = AttentionState{};
  out.kind = kind;
  copy_truncated(out.cwd, sizeof(out.cwd), doc["cwd"] | "");
  out.valid = true;
  return true;
}

bool attentionTick(AttentionState& s, uint32_t now_ms) {
  if (s.kind == ATTN_IDLE) return false;
  // Unsigned subtraction handles millis() rollover at ~49 days.
  uint32_t elapsed = now_ms - s.since_ms;
  if (elapsed <= kTimeoutMs) return false;
  s.kind = ATTN_IDLE;
  s.since_ms = now_ms;
  s.cwd[0] = '\0';
  return true;
}
```

- [ ] **Step 5.6: Run native tests, expect all pass**

```bash
cd firmware && pio test -e native -f test_attention
```

Expected: 12 tests pass.

- [ ] **Step 5.7: Commit**

```bash
git add firmware/src/attention.h firmware/src/attention.cpp \
        firmware/test/test_attention/test_attention.cpp \
        firmware/platformio.ini
git commit -m "firmware: parseAttentionJson + attentionTick + native tests"
```

---

## Task 6: HTTP `POST /attention` endpoint + manual smoke

**Files:**
- Modify: `firmware/src/main.cpp`

Wire the parser into the existing `WebServer` instance, store into a new global `g_attention` behind the existing `g_mutex`, set `g_dirty` so the next render tick redraws.

- [ ] **Step 6.1: Modify `firmware/src/main.cpp`**

Add the include near the top, alongside existing includes:

```cpp
#include "attention.h"
```

Add a new global below `g_state`:

```cpp
static AttentionState g_attention;
```

Add a new handler above `setup()`:

```cpp
static void handleAttention() {
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "no body"); return; }
  AttentionState parsed;
  if (!parseAttentionJson(server.arg("plain").c_str(), parsed)) {
    server.send(400, "text/plain", "bad json");
    return;
  }
  parsed.since_ms = millis();
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  g_attention = parsed;
  g_dirty = true;
  xSemaphoreGive(g_mutex);
  Serial.printf("[api] attention kind=%d cwd=%s\n", (int)parsed.kind, parsed.cwd);
  server.send(200, "text/plain", "ok");
}
```

In `setup()`, register the route alongside the existing `/data` route:

```cpp
  server.on("/data",      HTTP_POST, handleData);
  server.on("/attention", HTTP_POST, handleAttention);
```

**Do not** modify `loop()` in this task — the `renderTick` dispatch (which reads `g_attention`) is wired up in Task 7 alongside the signature change. Keeping the changes split this way means each commit leaves the firmware build green.

- [ ] **Step 6.2: Build firmware (verify no regressions)**

```bash
cd firmware && pio run -e rlcd42
```

Expected: builds clean. The `/attention` route is registered but inert until Task 7 makes the renderer use `g_attention`.

- [ ] **Step 6.3: Smoke test the parser path on hardware**

Flash the new build. From the Mac:

```bash
HOST="$(grep HOST mac/secrets.env | cut -d= -f2)"
curl -v -X POST -H 'Content-Type: application/json' \
  -d '{"ts":1745673120,"state":"WORKING","cwd":"/Users/rock/code/foo","session_id":"abc"}' \
  "http://${HOST}/attention"
```

Expected: HTTP 200; serial monitor shows `[api] attention kind=1 cwd=/Users/rock/code/foo`. Display still shows Screen A (renderer doesn't read `g_attention` yet).

Bad-payload check:

```bash
curl -v -X POST -H 'Content-Type: application/json' \
  -d '{"state":"WORKING"}' \
  "http://${HOST}/attention"
```

Expected: HTTP 400 `bad json`; `g_attention` on device unchanged from previous successful POST.

- [ ] **Step 6.4: Commit**

```bash
git add firmware/src/main.cpp
git commit -m "firmware: POST /attention handler + g_attention global"
```

---

## Task 7: `drawScreenB` + helpers + render dispatch (visual verification)

**Files:**
- Modify: `firmware/src/render.h`
- Modify: `firmware/src/render.cpp`

This task has no native test — `LGFX_Sprite` doesn't link cleanly under `platform = native`. Verification is by hardware photo for each of the three states.

- [ ] **Step 7.1: Update `firmware/src/render.h`**

Replace existing signature:

```cpp
#pragma once
#include <stdint.h>
#include "state.h"

void renderInit();
// ms_since_post: millis() - last_post_millis on caller side.
// `a` selects the layout: a.kind == ATTN_IDLE → Screen A (existing usage),
// otherwise → Screen B (attention overlay).
void renderTick(const UsageData& s, const AttentionState& a,
                bool stale, bool wifi_ok, uint32_t ms_since_post);
```

- [ ] **Step 7.2: Modify `firmware/src/render.cpp` — Screen B helpers**

Below the existing `drawWaiting()` function, add new helpers. Layout coordinates per spec §7:

```cpp
// ---- Screen B (attention) ----------------------------------------------

// Header for Screen B. Uses cwd basename in place of plan.
static void drawScreenBHeader(const AttentionState& a, bool wifi_ok) {
  d->fillRect(0, 0, 400, 44, BG);
  d->setTextColor(INK);

  // "CLAUDE" size 4 left, fake-bold via 1 px x-offset double-print.
  d->setTextSize(4);
  d->setCursor(8, 8);
  d->print("CLAUDE");
  int x_after = d->getCursorX();
  d->setCursor(9, 8);
  d->print("CLAUDE");

  // cwd basename right-aligned size 2 (truncate to 16 chars with leading "...").
  // basename of "" → "" (rendered nothing).
  const char* p = a.cwd;
  const char* basename_start = a.cwd;
  while (*p) {
    if (*p == '/') basename_start = p + 1;
    ++p;
  }
  char shown[20];
  size_t n = strlen(basename_start);
  if (n > 16) {
    shown[0] = '.'; shown[1] = '.'; shown[2] = '.';
    memcpy(shown + 3, basename_start + (n - 13), 13);
    shown[16] = '\0';
  } else {
    memcpy(shown, basename_start, n);
    shown[n] = '\0';
  }

  d->setTextSize(2);
  int pw = d->textWidth(shown);
  int px = 392 - pw;
  if (px < x_after + 12) px = x_after + 12;
  d->setCursor(px, 22);
  d->print(shown);

  if (!wifi_ok) {
    d->setTextSize(1);
    d->setCursor(360, 2);
    d->print("WiFi?");
  }

  d->drawFastHLine(0, 44, 400, INK);
}

// State-styled badge centered horizontally in y=80..180 strip.
//
//   WORKING  size 4, no box, no fill           — low visual weight
//   DONE     size 6, 1 px outline, no fill     — mid
//   WAITING  size 6, 2 px outline + filled,
//            text drawn in BG (reverse video)  — high
//
// The DONE/WAITING box geometry is identical so they read as the same
// thing with different urgency. WORKING fits inside the same y range
// without a box.
static void drawStateBadge(AttentionKind kind) {
  // Common box geometry (matches DONE/WAITING text width budget).
  const int box_x = 42, box_y = 80, box_w = 316, box_h = 100;
  const char* text = nullptr;
  int text_size = 0;
  switch (kind) {
    case ATTN_WORKING: text = "WORKING"; text_size = 4; break;
    case ATTN_DONE:    text = "DONE";    text_size = 6; break;
    case ATTN_WAITING: text = "WAITING"; text_size = 6; break;
    default: return;
  }

  if (kind == ATTN_WAITING) {
    d->fillRect(box_x,     box_y,     box_w,     box_h,     INK);
    d->drawRect(box_x - 1, box_y - 1, box_w + 2, box_h + 2, INK);  // 2 px border
    d->setTextColor(BG);
  } else if (kind == ATTN_DONE) {
    d->drawRect(box_x, box_y, box_w, box_h, INK);                  // 1 px border
    d->setTextColor(INK);
  } else {
    d->setTextColor(INK);
  }

  d->setTextSize(text_size);
  int tw = d->textWidth(text);
  int tx = box_x + (box_w - tw) / 2;
  // size N → glyph height ≈ 8*N; vertical-center inside box_h.
  int th = 8 * text_size;
  int ty = box_y + (box_h - th) / 2;
  d->setCursor(tx, ty);
  d->print(text);
  d->setTextColor(INK);  // restore default
}

// "<verb> <Nm>" centered at y=200, size 2.
static void drawDurationLine(AttentionKind kind, uint32_t elapsed_ms) {
  const char* verb = "";
  switch (kind) {
    case ATTN_WORKING: verb = "working"; break;
    case ATTN_DONE:    verb = "done";    break;
    case ATTN_WAITING: verb = "asking";  break;
    default: return;
  }
  uint32_t mins = elapsed_ms / 60000UL;
  char line[32];
  snprintf(line, sizeof(line), "%s %um", verb, (unsigned)mins);

  d->setTextColor(INK);
  d->setTextSize(2);
  int w = d->textWidth(line);
  int x = (400 - w) / 2;
  if (x < 0) x = 0;
  d->setCursor(x, 200);
  d->print(line);
}

// Single-line compact usage at y=280, size 2:
//   "5H 1.0M  4h30m   Wk 5.4M  6d 4h"
// If usage is invalid, render placeholders so the strip's height is preserved.
static void drawCompactUsageFooter(const UsageData& s) {
  d->drawFastHLine(0, 270, 400, INK);

  d->setTextColor(INK);
  d->setTextSize(2);
  d->setCursor(8, 280);

  if (!s.valid) {
    d->print("5H ----   Wk ----");
    return;
  }

  uint32_t now = s.ts;
  char tok5h[16], tokwk[16];
  formatTokens(s.tok_5h,     tok5h, sizeof(tok5h));
  formatTokens(s.tok_weekly, tokwk, sizeof(tokwk));
  char dur5h[16], durwk[16];
  if (s.reset_5h     > now) fmtDuration(s.reset_5h     - now, dur5h, sizeof(dur5h)); else dur5h[0] = '\0';
  if (s.reset_weekly > now) fmtDuration(s.reset_weekly - now, durwk, sizeof(durwk)); else durwk[0] = '\0';

  char line[64];
  snprintf(line, sizeof(line), "5H %s  %s   Wk %s  %s",
           tok5h, dur5h, tokwk, durwk);
  d->print(line);
}

// ---- Screen B top-level ------------------------------------------------

static void drawScreenB(const UsageData& u, const AttentionState& a,
                        uint32_t now_ms, bool wifi_ok) {
  drawScreenBHeader(a, wifi_ok);
  drawStateBadge(a.kind);
  uint32_t elapsed = now_ms - a.since_ms;
  drawDurationLine(a.kind, elapsed);
  drawCompactUsageFooter(u);
}
```

- [ ] **Step 7.3: Modify `renderTick` to dispatch**

Replace the existing `renderTick` body with:

```cpp
void renderTick(const UsageData& s, const AttentionState& a,
                bool stale, bool wifi_ok, uint32_t ms_since_post) {
  if (!d) return;

#if RENDER_DIAG_BARS
  // (unchanged diagnostic block)
  d->fillScreen(BG);
  drawDiagBlock( 20, "A");
  drawDiagBlock( 81, "B");
  drawDiagBlock(140, "C");
  drawDiagBlock(183, "D");
  drawDiagBlock(245, "E");
  displayCommit();
  return;
#endif

  d->fillScreen(BG);

  if (a.kind != ATTN_IDLE) {
    drawScreenB(s, a, millis(), wifi_ok);
    displayCommit();
    return;
  }

  // ---- Screen A (existing) ----
  drawHeader(s, wifi_ok);
  if (!s.valid) {
    drawWaiting();
    displayCommit();
    return;
  }

  uint32_t now = s.ts;
  char meta_l[80], meta_r[48];
  buildMeta5h(s, meta_l, sizeof(meta_l), meta_r, sizeof(meta_r), now);
  drawWindow(62, "5H BLOCK", s.tok_5h, s.started_5h, s.reset_5h, now,
             meta_l, meta_r);
  buildMetaWeekly(s, meta_l, sizeof(meta_l), meta_r, sizeof(meta_r), now);
  drawWindow(164, "WEEKLY", s.tok_weekly, s.started_weekly, s.reset_weekly,
             now, meta_l, meta_r);
  drawFooter(s, stale, ms_since_post);
  displayCommit();
}
```

- [ ] **Step 7.4: Wire the dispatch into `loop()` in `firmware/src/main.cpp`**

Replace the existing render-tick block in `loop()`:

```cpp
  static uint32_t last_render = 0;
  if (millis() - last_render >= 1000) {
    last_render = millis();
    bool wifi_ok = WiFi.status() == WL_CONNECTED;
    uint32_t age = millis() - g_last_post_ms;
    bool stale = age > 300000UL;

    xSemaphoreTake(g_mutex, portMAX_DELAY);
    UsageData      snap_u = g_state;
    AttentionState snap_a = g_attention;
    if (attentionTick(g_attention, millis())) {
      snap_a = g_attention;       // pick up the IDLE transition
      g_dirty = true;
    }
    xSemaphoreGive(g_mutex);

    renderTick(snap_u, snap_a, stale, wifi_ok, age);
  }
```

The previous block called `renderTick(snap, stale, wifi_ok, age)`; the new arg `snap_a` is the attention snapshot. `attentionTick` runs inside the mutex so the timeout-induced IDLE transition is observed atomically.

- [ ] **Step 7.5: Build firmware**

```bash
cd firmware && pio run -e rlcd42
```

Expected: builds clean. (Steps 7.1–7.4 commit as a single change; the build is only green once render.h, render.cpp, and main.cpp are all updated together.)

- [ ] **Step 7.6: Flash and visually verify each state on hardware**

Flash, then send each state via curl from the Mac and photograph the screen:

```bash
HOST="ai-desktop-buddy.local"
for state in WORKING DONE WAITING; do
  echo "=== $state ==="
  curl -fsS -X POST -H 'Content-Type: application/json' \
    -d "{\"ts\":1,\"state\":\"$state\",\"cwd\":\"/Users/rock/code/ai-desktop-buddy\"}" \
    "http://${HOST}/attention"
  echo
  sleep 8   # human reads the screen
done
# back to idle
curl -fsS -X POST -H 'Content-Type: application/json' \
  -d '{"ts":1,"state":"IDLE"}' "http://${HOST}/attention"
```

Manual checks (per spec §7):
1. `WORKING`: text-only, size 4, no box, centered. "working 0m" line below. Footer visible.
2. `DONE`: outlined box, size 6 text, identical box geometry to WAITING. "done 0m". Footer visible.
3. `WAITING`: filled black box with white "WAITING" text inside, size 6, slightly thicker border than DONE. "asking 0m". Footer visible.
4. After IDLE: returns to Screen A (existing usage layout) within 1–2 seconds.

If a footer overflows, switch the format string to a shorter form (e.g., drop one space) — the layout coordinate `y=280, size 2` has room for ~32 size-2 chars.

- [ ] **Step 7.7: Commit**

```bash
git add firmware/src/render.h firmware/src/render.cpp firmware/src/main.cpp
git commit -m "firmware: drawScreenB + state badge + compact footer + render dispatch"
```

---

## Task 8: KEY GPIO probe + notes

**Files:**
- Create: `docs/superpowers/notes/2026-04-30-key-gpio-probe.md`

Hardware research, no TDD applies. Identify the GPIO pin for the user-labeled "KEY" button (not BOOT, which is GPIO0).

- [ ] **Step 8.1: Read the Waveshare schematic / pinout**

Visit the official wiki (already bookmarked in memory: `https://docs.waveshare.net/ESP32-S3-RLCD-4.2/`). Locate the schematic / wiki for the three-button layout. Record the GPIO number for the KEY button.

- [ ] **Step 8.2: Write a one-off probe sketch (if pinout doc is ambiguous)**

Create a temporary file `firmware/src/main.cpp.probe` (do NOT replace main.cpp; copy main.cpp.probe over main.cpp temporarily, flash, observe, then restore). The probe sketch:

```cpp
#include <Arduino.h>

// Probe: configure all plausible button GPIOs as INPUT_PULLUP and report
// state on every change. Press each labeled button to identify pin.
static const int kPins[] = {0, 1, 2, 3, 14, 15, 16, 17, 18, 21, 35, 36, 37, 38, 39, 40};
static int kLast[sizeof(kPins)/sizeof(kPins[0])];

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("[probe] press buttons; expect a HIGH→LOW edge on the key's GPIO");
  for (size_t i = 0; i < sizeof(kPins)/sizeof(kPins[0]); ++i) {
    pinMode(kPins[i], INPUT_PULLUP);
    kLast[i] = digitalRead(kPins[i]);
  }
}

void loop() {
  for (size_t i = 0; i < sizeof(kPins)/sizeof(kPins[0]); ++i) {
    int v = digitalRead(kPins[i]);
    if (v != kLast[i]) {
      Serial.printf("GPIO%2d: %d -> %d\n", kPins[i], kLast[i], v);
      kLast[i] = v;
    }
  }
  delay(5);
}
```

Procedure: temporarily make main.cpp this content, flash, watch serial, press KEY repeatedly, note which GPIO toggles consistently. Press BOOT and PWR to verify they are different pins. Restore main.cpp.

- [ ] **Step 8.3: Document findings in `docs/superpowers/notes/2026-04-30-key-gpio-probe.md`**

Template:

```markdown
# KEY button pin verification (Waveshare ESP32-S3-RLCD-4.2)

**Date**: 2026-04-30

## Method
- Read pinout from [waveshare wiki](https://docs.waveshare.net/ESP32-S3-RLCD-4.2/).
- Confirmed via probe sketch (firmware/src/main.cpp.probe), pressing each
  labeled button and observing HIGH→LOW edges on serial.

## Result
- BOOT  → GPIO 0   (reserved; do not use as user input)
- PWR   → GPIO ??  (unused in firmware; battery PMIC interrupt)
- KEY   → GPIO ??  (user-dismiss button — used by key.cpp)

## Configuration
- pinMode(KEY_GPIO, INPUT_PULLUP)
- active-low (pressed = digital LOW)
- 30 ms software debounce
```

Fill in the `??`'s with actual pin numbers.

- [ ] **Step 8.4: Commit findings**

```bash
git add docs/superpowers/notes/2026-04-30-key-gpio-probe.md
git commit -m "docs: KEY button GPIO verified via probe sketch"
```

---

## Task 9: `key.cpp` driver + `main.cpp` integration

**Files:**
- Create: `firmware/src/key.h`
- Create: `firmware/src/key.cpp`
- Modify: `firmware/src/main.cpp`

The driver exposes two functions; debounce is software (sample once per loop, demand same value across 30 ms before reporting an edge).

- [ ] **Step 9.1: Write `firmware/src/key.h`**

```cpp
#pragma once
#include <stdint.h>

// Configure the KEY GPIO as INPUT_PULLUP. Call once from setup().
void keyInit();

// Edge-triggered, debounced. Returns true exactly once per *clean* press.
// Call every loop() iteration. Internally samples every ~5 ms; requires
// 30 ms of stable LOW after a HIGH→LOW transition before declaring a press.
bool keyPressedSinceLastCall();
```

- [ ] **Step 9.2: Write `firmware/src/key.cpp`**

Replace `KEY_GPIO_FROM_TASK_8` with the actual pin number identified in Task 8.

```cpp
#include "key.h"
#include <Arduino.h>

namespace {
constexpr int      kPin           = KEY_GPIO_FROM_TASK_8;
constexpr uint32_t kDebounceMs    = 30;
constexpr uint32_t kSamplePeriodMs = 5;

uint32_t g_last_sample_ms = 0;
int      g_last_raw       = HIGH;     // pulled-up idle
uint32_t g_stable_since_ms = 0;
int      g_stable_value    = HIGH;
bool     g_edge_pending    = false;   // true between debounced press and consume
}  // namespace

void keyInit() {
  pinMode(kPin, INPUT_PULLUP);
  g_last_raw = digitalRead(kPin);
  g_stable_value = g_last_raw;
  g_stable_since_ms = millis();
}

bool keyPressedSinceLastCall() {
  uint32_t now = millis();
  if (now - g_last_sample_ms >= kSamplePeriodMs) {
    g_last_sample_ms = now;
    int raw = digitalRead(kPin);
    if (raw != g_last_raw) {
      g_last_raw = raw;
      g_stable_since_ms = now;     // raw changed → restart stability timer
    } else if (raw != g_stable_value && (now - g_stable_since_ms) >= kDebounceMs) {
      // raw has been stable at a new value for >= debounce window
      g_stable_value = raw;
      if (raw == LOW) g_edge_pending = true;   // press edge HIGH→LOW
    }
  }
  if (g_edge_pending) {
    g_edge_pending = false;
    return true;
  }
  return false;
}
```

- [ ] **Step 9.3: Modify `firmware/src/main.cpp` — wire KEY**

Add include:

```cpp
#include "key.h"
```

In `setup()`, after `displayInit()` and `renderInit()`:

```cpp
  keyInit();
```

In `loop()`, immediately before the existing render block:

```cpp
  if (keyPressedSinceLastCall()) {
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_attention.kind = ATTN_IDLE;
    g_attention.since_ms = millis();
    g_attention.cwd[0] = '\0';
    g_dirty = true;
    xSemaphoreGive(g_mutex);
    Serial.println("[key] dismissed → IDLE");
  }
```

- [ ] **Step 9.4: Build, flash, test KEY on hardware**

```bash
cd firmware && pio run -e rlcd42 -t upload && pio device monitor
```

Test sequence:
1. Send `WAITING` via curl. Verify Screen B WAITING.
2. Press KEY once. Verify serial prints `[key] dismissed → IDLE`. Verify Screen A returns within 1 second.
3. Press KEY again with no active attention. Verify serial still prints (no harm) and screen unchanged.
4. Hold KEY pressed for 5 seconds. Verify only one `[key] dismissed` message (no key-repeat).

- [ ] **Step 9.5: Commit**

```bash
git add firmware/src/key.h firmware/src/key.cpp firmware/src/main.cpp
git commit -m "firmware: KEY button debounce + force IDLE on press"
```

---

## Task 10: End-to-end hardware walk-through

**Files:**
- Modify: `docs/superpowers/notes/2026-04-30-key-gpio-probe.md` (append e2e log) OR create new note

This is the spec §11.3 acceptance test. Real device, real Claude Code session, hand-driven.

- [ ] **Step 10.1: Install hooks**

```bash
mac/install-hooks.sh
cat ~/.claude/settings.json | jq .hooks
```

Expected output: 4 entries, one each for UserPromptSubmit / Stop / Notification / SessionEnd, all pointing to `mac/hooks/*.sh`.

- [ ] **Step 10.2: Verify cold boot → Screen A**

Power-cycle the device. Wait for the 60 s `push-usage.sh` cycle. Screen A appears with current usage.

- [ ] **Step 10.3: Verify WORKING transition**

In a terminal:
```bash
claude
> Make a one-line change to README.md
```

Within 2 seconds of pressing Enter, the device should switch to Screen B WORKING; the duration line should tick `working 0m` → `working 1m` over a minute.

- [ ] **Step 10.4: Verify DONE transition**

Wait for Claude's response to complete. Screen should transition to Screen B DONE within ~2 seconds; duration resets to `done 0m`.

- [ ] **Step 10.5: Verify WORKING again on follow-up prompt**

Type a follow-up `>` prompt. Screen returns to WORKING within 2 s.

- [ ] **Step 10.6: Verify WAITING transition (permission prompt)**

Trigger a permission prompt — for example:
```
> run `ls /` in a Bash tool
```
Claude will ask permission. Verify Screen B WAITING within 2 s; reverse-video filled box; line reads `asking 0m`.

Across-the-room visual check (~1.5 m): WAITING is clearly distinguishable from DONE (filled vs outline).

- [ ] **Step 10.7: Verify KEY dismiss**

Approve the permission. Re-trigger DONE. Press KEY on the device. Screen returns to A within 1 second.

- [ ] **Step 10.8: Verify 15-min timeout**

Trigger DONE. Leave the terminal idle for >15 minutes. After ~15 min, Screen A should re-appear automatically.

- [ ] **Step 10.9: Verify SessionEnd**

Re-trigger any state. In Claude, type `/exit`. Screen should return to A within ~2 seconds.

- [ ] **Step 10.10: Record the walk-through results**

Append to `docs/superpowers/notes/2026-04-30-key-gpio-probe.md` (or new file `docs/superpowers/notes/2026-04-30-attention-e2e-log.md`):

```markdown
# Attention alert end-to-end log

**Date**: 2026-04-30 (or actual date when run)
**Device firmware sha**: <git rev-parse --short HEAD>
**Result**: pass / fail

| Step | Expected                              | Observed                    |
|------|---------------------------------------|-----------------------------|
| 10.2 | Cold boot → Screen A                  |                             |
| 10.3 | UserPromptSubmit → WORKING ≤2 s       |                             |
| 10.4 | Stop → DONE                           |                             |
| 10.5 | UserPromptSubmit → WORKING            |                             |
| 10.6 | Notification → WAITING (across room)  |                             |
| 10.7 | KEY → IDLE ≤1 s                       |                             |
| 10.8 | 15-min timeout → IDLE                 |                             |
| 10.9 | SessionEnd → IDLE ≤2 s                |                             |
```

- [ ] **Step 10.11: Commit**

```bash
git add docs/superpowers/notes/
git commit -m "docs: end-to-end attention alert walk-through verified"
```

---

## Self-review checklist (run after final commit)

Before declaring done, verify against the spec:

- [ ] §3 hard constraints honored: no Mac listening port, no device-initiated requests, no Anthropic API calls. Hooks → curl → /attention → device only.
- [ ] §6.1 wire schema: ts required, state required + enum, cwd optional + truncated, session_id stored only on the wire (not on device).
- [ ] §7.2 badge geometry: WORKING (size 4, no box), DONE (size 6, 1 px outline), WAITING (size 6, 2 px outline + filled reverse-video). All same y range.
- [ ] §8.2 transitions: every event in the table has a code path. KEY → IDLE. 15 min timeout → IDLE.
- [ ] §10 error handling: 400 on bad payload, hooks don't block Claude on curl failure, ESP32 reboot starts at IDLE.
- [ ] §11 testing: bats tests for hooks (Task 1, 3); native tests for parser + tick (Task 5); hardware visual + e2e (Tasks 7, 9, 10).

If a row above can't be ticked, file a follow-up task before claiming the feature complete.
