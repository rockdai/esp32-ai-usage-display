#!/usr/bin/env bash
# Fires on Claude Code's Notification hook (needs user input) → state WAITING.
. "$(dirname "$0")/_lib.sh"
post_attention WAITING
