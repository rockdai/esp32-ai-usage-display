#pragma once
#include "state.h"

// Returns true and populates `out` on valid payload; false on malformed.
bool parseUsageJson(const char* body, UsageData& out);
