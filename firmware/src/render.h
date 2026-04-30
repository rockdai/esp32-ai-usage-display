#pragma once
#include <stdint.h>
#include "state.h"

void renderInit();
// ms_since_post: millis() - last_post_millis on caller side.
// `a` selects the layout: a.kind == ATTN_IDLE → Screen A (existing usage),
// otherwise → Screen B (attention overlay).
void renderTick(const UsageData& s, const AttentionState& a,
                bool stale, bool wifi_ok, uint32_t ms_since_post);
