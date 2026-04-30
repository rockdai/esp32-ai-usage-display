#!/usr/bin/env bash
# Fires on Claude Code's Stop hook (response complete) → state DONE.
. "$(dirname "$0")/_lib.sh"
post_attention DONE
