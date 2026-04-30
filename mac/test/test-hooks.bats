#!/usr/bin/env bats
# Verifies _lib.sh post_attention builds the right payload and calls curl.

setup() {
  STAGE="$BATS_TEST_TMPDIR"
  mkdir -p "$STAGE/hooks" "$STAGE/stub"
  cp "$BATS_TEST_DIRNAME/../hooks/_lib.sh" "$STAGE/hooks/_lib.sh"
  echo "HOST=test.local" > "$STAGE/secrets.env"

  # Stub curl: capture argv and the value of --data to files; exit 0.
  cat > "$STAGE/stub/curl" <<STUB
#!/bin/sh
# Capture argv (space-joined for substring grep) and the value of --data.
printf '%s ' "\$@" > "$STAGE/curl-argv"
echo >> "$STAGE/curl-argv"
prev=""
for a in "\$@"; do
  if [ "\$prev" = "--data" ]; then
    printf '%s' "\$a" > "$STAGE/curl-body"
    break
  fi
  prev="\$a"
done
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
