# Attention alert end-to-end walkthrough log

**Date**: 2026-04-30
**Hardware**: Waveshare ESP32-S3-RLCD-4.2 (user's actual unit, USB-attached)
**Firmware sha (after fixes)**: `8d8c966`
**Result**: pass with two scope changes (KEY hardware absent; 15-min timeout not live-verified)

## Method

Synthetic test only. Hooks were intentionally NOT installed during this session
to avoid hooks firing on the controller's own assistant turns and polluting the
state. Each Screen B state was driven by a `curl POST /attention` from the
controller; the user confirmed the visual on the panel.

## Step results

| Step | Expected | Observed |
|------|----------|----------|
| Cold flash + boot | New firmware loads, mDNS advertises, push-usage.sh repopulates `g_state` within 60 s | Boot OK; mDNS resolved (`ai-desktop-buddy.local` → `192.168.20.206`); manual `mac/push-usage.sh` run delivered fresh `g_state` |
| `POST /attention {state:WORKING}` | Screen B with `WORKING` text size 4, no box | Confirmed by user |
| `POST /attention {state:DONE}` | Screen B with `DONE` size 6, 1 px outlined box | Confirmed by user |
| `POST /attention {state:WAITING}` | Screen B with reverse-video filled `WAITING` block, distinguishable from DONE across the room | Confirmed by user |
| `POST /attention {state:IDLE}` | Returns to Screen A within ≤1 s | Confirmed by user |

## Issues found and fixed during e2e

### 1. Compact-usage footer overflow (commit `1eaa1e8`)

The footer format `5H 60.2M  4h30m   Wk 491.1M  6d 4h` was ~408 px wide at
size 2 (panel is 400 px wide). Default LovyanGFX text wrapping pushed the
overflow to a second line at y=296, which appeared as garbled text below
the `5H` label.

Fix: introduced `formatTokensCompact()` (integer-M, no decimal), changed the
format string to `5H %s %s  Wk %s %s`, and set `setTextWrap(false)` as a
defensive guard so any future overflow clips at the right edge instead of
wrapping. Worst-case line is now ~30 chars (~360 px) with margin to spare.

### 2. Screen B header hierarchy was inverted (commit `1eaa1e8`)

Original design had "CLAUDE" size 4 dominant on the left and the cwd
basename size 2 tucked at the right. The cwd is the actionable info
("which window do I switch to") — the hierarchy was wrong.

Fix: project basename promoted to size 3 left-aligned (still ~3× the
visual weight of the original size 2), "CLAUDE" branding label removed
entirely. The "WiFi?" indicator surfaces only when the network is down,
in the previously-unused top-right corner.

Subsequent user feedback wanted the small "claude" tag dropped entirely
too — done in the same commit (header simplified to project name plus
optional Wi-Fi indicator).

## Scope changes

### KEY-dismiss feature: removed (commit `8d8c966`)

The user's board has only the BOOT button; the third "KEY" button described
in the Waveshare reference docs and demo source (GPIO 18) is absent on this
revision. The KEY-dismiss path was therefore deleted from firmware.
Clearance for v1.0 happens via SessionEnd hook + 15-min `attentionTick`
timeout only.

If the user later wires a tactile button to GPIO 18, restoring KEY support
is reverting two diffs: the deletion of `firmware/src/key.{h,cpp}` and the
removal of the wires in `main.cpp`. See commit `8d8c966`'s parent.

## Not exercised in this session

- **15-min `attentionTick` timeout.** Verified by unit test (`test_tick_past_timeout_goes_idle`); not live-tested because waiting 15 minutes was not worth the time cost. If a regression is suspected later, kick a state into `WORKING/DONE/WAITING`, walk away, and check that Screen A is back after 16 minutes.
- **Real Claude Code session driving the hooks.** Hooks remain not installed (`mac/install-hooks.sh` not run). The user can install them at any time outside this session; the synthetic tests above already exercised every endpoint path the hooks would hit.
- **Multi-machine or multi-session conflict.** Last-event-wins behavior is documented in the spec; no live test of two simultaneous `claude` terminals.

## Sign-off

State machine paths all verified end-to-end with synthetic events; UI
issues found during the walkthrough are fixed and re-verified on hardware;
out-of-scope items (KEY, 15-min timeout live test, real session) are
documented above. Ready for final code review (Task 11).
