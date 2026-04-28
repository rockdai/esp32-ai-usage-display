#include "api.h"
#include <ArduinoJson.h>
#include <string.h>

bool parseUsageJson(const char* body, UsageData& out) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  if (!doc["ts"].is<uint32_t>()) return false;

  // Required block_5h fields
  if (!doc["block_5h"]["used_tokens"].is<uint64_t>()) return false;
  if (!doc["block_5h"]["started_at"].is<uint32_t>())  return false;
  if (!doc["block_5h"]["resets_at"].is<uint32_t>())   return false;
  // Required weekly fields
  if (!doc["weekly"]["used_tokens"].is<uint64_t>()) return false;
  if (!doc["weekly"]["started_at"].is<uint32_t>())  return false;
  if (!doc["weekly"]["resets_at"].is<uint32_t>())   return false;

  out.ts = doc["ts"];
  strlcpy(out.plan, doc["plan"] | "", sizeof(out.plan));

  // 5h block
  JsonObjectConst b = doc["block_5h"].as<JsonObjectConst>();
  out.tok_5h     = b["used_tokens"];
  out.started_5h = b["started_at"];
  out.reset_5h   = b["resets_at"];
  if (b["cost_usd"].is<double>())       { out.cost_5h_usd   = b["cost_usd"];      out.cost_5h_present = true; }
  if (b["messages"].is<uint32_t>())     { out.msgs_5h       = b["messages"];      out.msgs_5h_present = true; }
  if (b["burn_rate_tpm"].is<uint32_t>()){ out.burn_tpm      = b["burn_rate_tpm"]; out.burn_present    = true; }

  // weekly
  JsonObjectConst w = doc["weekly"].as<JsonObjectConst>();
  out.tok_weekly     = w["used_tokens"];
  out.started_weekly = w["started_at"];
  out.reset_weekly   = w["resets_at"];
  if (w["cost_usd"].is<double>())   { out.cost_weekly_usd  = w["cost_usd"]; out.cost_weekly_present = true; }
  if (w["messages"].is<uint32_t>()) { out.msgs_weekly      = w["messages"]; out.msgs_weekly_present = true; }

  // today (optional whole object)
  if (!doc["today"].isNull()) {
    JsonObjectConst t = doc["today"].as<JsonObjectConst>();
    out.tok_today      = t["tokens"]   | (uint64_t)0;
    out.msgs_today     = t["messages"] | (uint32_t)0;
    out.cost_today_usd = t["cost_usd"] | 0.0;
    out.today_present  = true;
  }

  out.valid = true;
  return true;
}
