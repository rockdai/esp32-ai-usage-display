#pragma once
#include <stdint.h>
#include "state.h"

void renderInit();
// ms_since_post: millis() - last_post_millis on caller side
void renderTick(const UsageData& s, bool stale, bool wifi_ok, uint32_t ms_since_post);
