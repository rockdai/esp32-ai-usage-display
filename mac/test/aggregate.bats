#!/usr/bin/env bats

setup() {
  ROOT="$(cd "$BATS_TEST_DIRNAME/.." && pwd)"
  PUSH="$ROOT/push-usage.sh"
  FIX="$ROOT/fixtures/sample"
  NOW=1777735800   # 2026-04-26T15:30:00Z
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
  [ "$(jq -r '.block_5h.started_at'  <<<"$output")" = "1777734000" ]
  [ "$(jq -r '.block_5h.resets_at'   <<<"$output")" = "1777752000" ]
}

@test "weekly.started_at derives from earliest user event in trailing 168h" {
  run run_push
  [ "$status" -eq 0 ]
  [ "$(jq -r '.weekly.started_at' <<<"$output")" = "1777718313" ]
  [ "$(jq -r '.weekly.resets_at'  <<<"$output")" = "1778323113" ]
}

@test "weekly.used_tokens sums daily entries within the window" {
  run run_push
  [ "$(jq -r '.weekly.used_tokens' <<<"$output")" = "5400000" ]
  [ "$(jq -r '.weekly.cost_usd'    <<<"$output")" = "23.69" ]
}

@test "today fields populate from current local date in daily.json" {
  run run_push
  [ "$(jq -r '.today.tokens'   <<<"$output")" = "3100000" ]
  [ "$(jq -r '.today.cost_usd' <<<"$output")" = "13.91" ]
}

@test "no quota fields leak into output" {
  run run_push
  [ "$(jq -e '.block_5h | has("percent")'      <<<"$output")" = "false" ]
  [ "$(jq -e '.block_5h | has("limit_tokens")' <<<"$output")" = "false" ]
  [ "$(jq -e '.weekly   | has("percent")'      <<<"$output")" = "false" ]
  [ "$(jq -e '.weekly   | has("limit_tokens")' <<<"$output")" = "false" ]
}

@test "plan and ts present at root" {
  run run_push
  [ "$(jq -r '.plan' <<<"$output")" = "Max 5x" ]
  [ "$(jq -r '.ts'   <<<"$output")" = "1777735800" ]
}
