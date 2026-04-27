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
