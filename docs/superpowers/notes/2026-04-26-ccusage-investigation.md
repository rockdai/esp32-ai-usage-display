# ccusage capability + Max 5x plan limits — investigation

Date: 2026-04-26
Author: Task 1 of `docs/superpowers/plans/2026-04-26-esp32-ai-usage-display.md`
ccusage version surveyed: **18.0.11** (via `npx ccusage@latest`)
Host: macOS, user on Claude Max 5x.

This note de-risks spec §11.1 (does ccusage natively expose the windows we
need) and §11.2 (what are the Max 5x token caps). The downstream consumer is
`mac/push-usage.sh` (Task 5).

---

## ccusage subcommands surveyed

`npx ccusage@latest --help` lists six subcommands:

| subcommand   | purpose (per `--help`)                                       | useful for us? |
|--------------|--------------------------------------------------------------|----------------|
| `daily`      | Usage report grouped by date                                 | Yes — fallback source for weekly aggregation |
| `monthly`    | Usage report grouped by month                                | No |
| `weekly`     | Usage report grouped by **calendar** week (Sun–Sat default)  | **No** for Anthropic's rolling weekly window — see below |
| `session`    | Usage report grouped by conversation session                 | No |
| `blocks`     | Usage report grouped by **5-hour billing blocks**            | **Yes — primary source** |
| `statusline` | Compact status line for Claude Code hooks (Beta)             | No |

All subcommands accept `--json`, `-s/--since`, `-u/--until`, `-z/--timezone`,
`-q/--jq`, etc. `--json` produces structured output suitable for `jq` parsing.

### `blocks --active --json` — sample output (real, on user's Mac)

```json
{
  "blocks": [
    {
      "id": "2026-04-26T15:00:00.000Z",
      "startTime": "2026-04-26T15:00:00.000Z",
      "endTime":   "2026-04-26T20:00:00.000Z",
      "actualEndTime": "2026-04-26T15:29:49.215Z",
      "isActive": true,
      "isGap": false,
      "entries": 11,
      "tokenCounts": {
        "inputTokens": 30,
        "outputTokens": 12274,
        "cacheCreationInputTokens": 173598,
        "cacheReadInputTokens": 831960
      },
      "totalTokens": 1017862,
      "costUSD": 1.808,
      "models": ["claude-opus-4-7"],
      "burnRate":  { "tokensPerMinute": 82684.1, "costPerHour": 8.81 },
      "projection":{ "totalTokens": 23353855, "totalCost": 41.48, "remainingMinutes": 270 }
    }
  ]
}
```

Key observations:
- Blocks are aligned on clock boundaries (e.g. 10:00, 15:00, 20:00 UTC), not
  rolling from "now − 5h". A block runs from `startTime` to `endTime =
  startTime + 5h`. **There is exactly one `isActive: true` block at any time**,
  and its `endTime` is the reset clock we want to display on the device.
- `totalTokens` is the sum of `inputTokens + outputTokens +
  cacheCreationInputTokens + cacheReadInputTokens` — i.e. the same definition
  ccusage uses everywhere. (For 5h cap percentages we will use this same total.)
- `--recent` returns the last 3 days of blocks plus the active one, including
  synthetic `isGap: true` blocks for idle stretches (`totalTokens: 0`).

### `daily --json` — sample shape

```json
{
  "daily": [
    { "date": "2026-04-26", "inputTokens": 385, "outputTokens": 111207,
      "cacheCreationTokens": 498639, "cacheReadTokens": 5330375,
      "totalTokens": 5940606, "totalCost": 8.56,
      "modelsUsed": ["claude-opus-4-7"],
      "modelBreakdowns": [ { "modelName": "...", "inputTokens": ..., ... } ] }
  ],
  "totals": { "inputTokens": ..., "outputTokens": ..., "cacheCreationTokens": ...,
              "cacheReadTokens": ..., "totalTokens": ..., "totalCost": ... }
}
```

### `weekly --json` — sample shape

Same shape as `daily`, but keyed by `"week": "2026-04-26"` (the start-of-week
date). Default start-of-week is **Sunday**, configurable via
`--start-of-week monday|tuesday|...`. **This is calendar-week aggregation, not
the Anthropic 7-day rolling window** that resets on the user's first session
of the period.

---

## Field map

What we need vs. what ccusage gives us:

| Need                                       | Subcommand               | JSON path                                                        | Status |
|--------------------------------------------|--------------------------|------------------------------------------------------------------|--------|
| (a) Tokens within trailing 5 h             | `blocks --active --json` | `.blocks[0].totalTokens` (when `isActive` and `!isGap`)          | **Native** |
| (b) Tokens within current weekly window    | (none directly)          | Compute by summing `daily --json` entries within the rolling 7-day window | **Gap — local aggregation needed** |
| (c) Per-day breakdown                      | `daily --json`           | `.daily[].{date,totalTokens,totalCost,modelsUsed}`               | **Native** |
| (d) Current 5 h block reset time           | `blocks --active --json` | `.blocks[0].endTime` (ISO-8601 UTC)                              | **Native** |

Bonus fields ccusage gives us for free (we may surface on the device later):
- `.blocks[0].burnRate.tokensPerMinute` — could power a "burn rate" indicator.
- `.blocks[0].projection.totalTokens` / `.totalCost` — projected end-of-block
  spend if current rate continues.
- `.blocks[0].costUSD` — per-block USD cost.

### The weekly gap explained

`ccusage weekly` groups by calendar week (Sun–Sat, configurable), but the
Anthropic Max plan's weekly limit is a **rolling 7-day window that resets 7
days after the first session of the period** (per the Max-plan support article
quoted below). ccusage 18.0.11 has no flag to emit "trailing 168 h tokens".

Two practical paths for `push-usage.sh`:

1. **Approximation path (recommended for v1):** sum `daily.totalTokens` for
   the trailing 7 calendar days (today + 6 prior). This is what most community
   trackers do. It will be off by up to ~24 h vs. Anthropic's true reset clock
   but is monotonic and easy to reason about.
2. **Exact path (deferred):** parse `~/.claude/projects/**/*.jsonl` directly
   and find the oldest event in the last 168 h to derive the true reset
   timestamp. Out of scope for v1 — flagged as an open question.

---

## Max 5x plan limits

**Bottom line: Anthropic does not publish exact token-count limits for the Max
5x plan. They publish *prompt-count* and *Sonnet/Opus hour* ranges instead,
and these ranges are wide.**

Sources consulted (in spec preference order):

1. **Official Anthropic docs** — `support.claude.com` Max-plan article
   ("What is the Max plan?", id 11049741) confirms the *structure* of the
   limits but does not give numbers:
   > "Max plans also have two weekly usage limits: one that applies across
   > all models and another for Sonnet models only. Both limits reset seven
   > days after your session starts."

   The Claude Code rate-limits doc
   (`docs.claude.com/en/docs/claude-code/rate-limits` →
   `code.claude.com/docs/en/rate-limits`) is currently 404 / redirect-loop
   from this environment, so I could not pull definitive numbers.

2. **Claude Code `/status` output on this machine** — not captured by this
   research task (would require an interactive session). Flagged as an open
   question; running `/status` once will give the user-specific authoritative
   numbers and we can hard-code or read them from a config file.

3. **Empirical / community sources** — converging numbers:
   - 5-hour window: **~225 messages per 5 h** for Max 5x (vs. ~45 for Pro,
     ~900 for Max 20x). Source: usagebar.com, intuitionlabs.ai.
   - Weekly window: published as **140–280 hours of Sonnet 4 per week** plus
     a separate, smaller Opus allowance. Source: intuitionlabs.ai citing
     Anthropic's July-2025 weekly-limits announcement.
   - Anthropic explicitly does not publish token totals for consumer plans —
     they vary with conversation length, cache hit rate, and model choice.

### Placeholder constants for `push-usage.sh` (TBC)

Because the project's display divides "tokens used / cap × 100%", and
Anthropic's published cap is in messages/hours not tokens, **we will use
empirically-calibrated placeholders, marked TBC, until the user runs `/status`
or hits a real cap:**

```bash
# mac/push-usage.sh — TBC: confirm against /status output or first cap-hit
MAX5X_5H_TOKEN_CAP=19000000        # 19M tokens/5h — order-of-magnitude guess from
                                   # community ccusage screenshots; ~225 messages
                                   # × ~80k tokens/message average.
MAX5X_WEEKLY_TOKEN_CAP=440000000   # 440M tokens/7d — derived from "140 h Sonnet"
                                   # at a conservative throughput.
```

These numbers are **deliberately rough** and exist solely so the device shows
a non-zero percentage on day 1. The real plan once the device is running:
1. Run Claude Code's `/status` and read the Max 5x exact caps; or
2. Watch for the first time `/status` reports "5h limit reached" and back-fill
   the constant from `blocks --active --json` `totalTokens` at that moment.

Whichever number we settle on lives as a **single config constant** in
`push-usage.sh` (or `secrets.env`). The display will read it from the JSON
the script pushes — the firmware itself does not hard-code plan caps.

---

## Aggregation decision for push-usage.sh

**Hybrid: native ccusage for the 5 h window, local aggregation for weekly.**

| Field on the device                  | How `push-usage.sh` computes it                                        |
|--------------------------------------|-------------------------------------------------------------------------|
| `block_5h.tokens_used`               | `ccusage blocks --active --json \| jq '.blocks[0].totalTokens // 0'`  |
| `block_5h.reset_at_iso`              | `ccusage blocks --active --json \| jq -r '.blocks[0].endTime // empty'` |
| `block_5h.percent`                   | `tokens_used / MAX5X_5H_TOKEN_CAP * 100`                                |
| `weekly.tokens_used`                 | Sum of `daily.totalTokens` for last 7 calendar days from `ccusage daily --json` |
| `weekly.reset_at_iso`                | Approximated as `(oldest day in window).date + 7 days @ 00:00 local`   |
| `weekly.percent`                     | `tokens_used / MAX5X_WEEKLY_TOKEN_CAP * 100`                            |
| `burn_rate_tokens_per_min` (optional)| `.blocks[0].burnRate.tokensPerMinute`                                  |

Edge cases the script must handle (testable in Task 5 with bats):

- `ccusage blocks --active --json` may return `{ "blocks": [] }` if there is
  no active block (idle for >5 h since last session). Treat as `tokens_used: 0`.
- `isGap: true` blocks have `totalTokens: 0` — skip them when summing the
  trailing 5 h. (For `--active` this can't happen, but for `--recent` it can.)
- ccusage prints `WARN`/`ℹ` lines on stderr about pricing fetch — `push-usage.sh`
  must read **stdout only** (`2>/dev/null` or pipe through `jq`).
- First-run cold start downloads the package via npx (~2–3 s on this machine);
  subsequent runs are warm-cached. The launchd interval (e.g. 30 s) is far
  longer than that, so we ignore it.

---

## Open questions

1. **Authoritative Max 5x caps.** Run Claude Code's `/status` on the user's
   machine once and record the exact 5h and weekly caps. This replaces both
   placeholder constants. (Cannot be done from this research task — needs an
   interactive Claude Code session.)
2. **Anthropic weekly-window reset clock.** ccusage's `weekly` is calendar-week
   aggregation, not Anthropic's "7 days from first session" rolling window.
   For v1 we approximate with trailing-7-day calendar days; if this drifts
   visibly from `/status` we will need to either (a) parse `~/.claude/projects`
   JSONL directly, or (b) ignore weekly altogether and show only the 5h window.
3. **Multi-account / Codex CLI.** ccusage only sees Claude Code logs in
   `~/.claude/`. The user also runs Codex CLI (Max 20x); a parallel investigation
   is needed for Codex usage data — explicitly out of scope for Task 1, slated
   for the future Codex split-screen.
4. **Cost vs. token framing.** `costUSD` is consistent across the JSON outputs
   and might be more meaningful than tokens for a glanceable display, but it
   does not map onto the plan's "messages/hours" cap either. For v1 we stick
   with `tokens / cap` ratios and keep `costUSD` as a footer/debug field.
