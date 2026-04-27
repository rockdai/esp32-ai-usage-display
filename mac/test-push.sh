#!/usr/bin/env bash
# Usage: ./test-push.sh fixtures/edge-cases/idle.json
# Posts a fixture to the device for manual visual verification.
set -euo pipefail
[ $# -eq 1 ] || { echo "usage: $0 <fixture.json>" >&2; exit 64; }
# shellcheck disable=SC1091
. "$(dirname "$0")/secrets.env"
curl -fsS -X POST -H 'Content-Type: application/json' \
  --data @"$1" "http://${HOST}/data"
echo
