#include "attention.h"
#include <ArduinoJson.h>
#include <string.h>

namespace {

constexpr uint32_t kTimeoutMs = 15UL * 60UL * 1000UL;

// Maps wire literal to enum. Returns true on match.
bool parseStateLiteral(const char* s, AttentionKind& out) {
  if (!s) return false;
  if (strcmp(s, "WORKING") == 0) { out = ATTN_WORKING; return true; }
  if (strcmp(s, "DONE")    == 0) { out = ATTN_DONE;    return true; }
  if (strcmp(s, "WAITING") == 0) { out = ATTN_WAITING; return true; }
  if (strcmp(s, "IDLE")    == 0) { out = ATTN_IDLE;    return true; }
  return false;
}

// Truncating copy: at most dst_size-1 chars from src, always null-terminated.
// Mirrors strlcpy semantics so the platformio native env (no BSD strlcpy)
// builds without extra deps.
void copy_truncated(char* dst, size_t dst_size, const char* src) {
  if (dst_size == 0) return;
  if (!src) { dst[0] = '\0'; return; }
  size_t n = strlen(src);
  if (n >= dst_size) n = dst_size - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

}  // namespace

bool parseAttentionJson(const char* body, AttentionState& out) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  if (!doc["ts"].is<uint32_t>())   return false;

  const char* state_str = doc["state"] | (const char*)nullptr;
  AttentionKind kind;
  if (!parseStateLiteral(state_str, kind)) return false;

  // Reset to defaults so leftover state from previous parse can't leak.
  out = AttentionState{};
  out.kind = kind;
  copy_truncated(out.cwd, sizeof(out.cwd), doc["cwd"] | "");
  out.valid = true;
  return true;
}

bool attentionTick(AttentionState& s, uint32_t now_ms) {
  if (s.kind == ATTN_IDLE) return false;
  // Unsigned subtraction handles millis() rollover at ~49 days.
  uint32_t elapsed = now_ms - s.since_ms;
  if (elapsed <= kTimeoutMs) return false;
  s.kind = ATTN_IDLE;
  s.since_ms = now_ms;
  s.cwd[0] = '\0';
  return true;
}
