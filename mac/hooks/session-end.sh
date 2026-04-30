#!/usr/bin/env bash
# Fires on Claude Code's SessionEnd hook (session closing) → state IDLE.
. "$(dirname "$0")/_lib.sh"
post_attention IDLE
