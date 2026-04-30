#pragma once
#include "state.h"

// Parse the body of POST /attention. Returns true and populates `out` on
// success; false on any validation failure. On false return, `out` is in
// an indeterminate state (caller must not consume it).
//
// Required fields: "ts" (uint32), "state" (one of "WORKING" | "DONE"
// | "WAITING" | "IDLE"; case-sensitive).
//
// Optional fields: "cwd" (string; truncated to 63 chars on store),
// "session_id" (ignored in v1).
//
// `out.since_ms` is NOT set here — the HTTP handler stamps it with the
// device's current millis() *after* a successful parse.
bool parseAttentionJson(const char* body, AttentionState& out);

// Returns true if the timeout caused a transition to ATTN_IDLE.
// Called every render tick. Uses `(uint32_t)(now_ms - s.since_ms)` so
// millis() rollover is handled without conditionals.
bool attentionTick(AttentionState& s, uint32_t now_ms);
