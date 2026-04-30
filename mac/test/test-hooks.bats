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
