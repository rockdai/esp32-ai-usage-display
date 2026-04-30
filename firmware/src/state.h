#pragma once
#include <stdint.h>

struct UsageData {
  uint32_t ts = 0;
  char     plan[16] = {0};

  // 5h block (required: started_at, resets_at, used_tokens)
  uint64_t tok_5h        = 0;
  uint32_t started_5h    = 0;
  uint32_t reset_5h      = 0;
  double   cost_5h_usd   = 0.0;
  uint32_t msgs_5h       = 0;
  uint32_t burn_tpm      = 0;
  bool     burn_present  = false;
  bool     cost_5h_present = false;
  bool     msgs_5h_present = false;

  // weekly (required: started_at, resets_at, used_tokens)
  uint64_t tok_weekly       = 0;
  uint32_t started_weekly   = 0;
  uint32_t reset_weekly     = 0;
  double   cost_weekly_usd  = 0.0;
  uint32_t msgs_weekly      = 0;
  bool     cost_weekly_present = false;
  bool     msgs_weekly_present = false;

  // today (whole object optional)
  uint64_t tok_today       = 0;
  uint32_t msgs_today      = 0;
  double   cost_today_usd  = 0.0;
  bool     today_present   = false;

  bool     valid = false;
};

// ---- Attention (Claude Code activity) -----------------------------------

enum AttentionKind : uint8_t {
  ATTN_IDLE    = 0,   // no active session — render Screen A (usage)
  ATTN_WORKING = 1,   // user submitted prompt; Claude processing
  ATTN_DONE    = 2,   // Claude finished; user should look
  ATTN_WAITING = 3,   // Claude needs input (permission prompt etc)
};

struct AttentionState {
  AttentionKind kind     = ATTN_IDLE;
  uint32_t      since_ms = 0;     // millis() at entry into current kind
  char          cwd[64]  = {0};   // truncated to 63 chars + null
  bool          valid    = false; // false until first event lands
};
