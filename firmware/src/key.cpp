#include "key.h"
#include <Arduino.h>

namespace {
constexpr int      kPin            = 18;     // Waveshare ESP32-S3-RLCD-4.2 silkscreen "KEY"
constexpr uint32_t kDebounceMs     = 30;
constexpr uint32_t kSamplePeriodMs = 5;

uint32_t g_last_sample_ms = 0;
int      g_last_raw       = HIGH;     // pulled-up idle
uint32_t g_stable_since_ms = 0;
int      g_stable_value    = HIGH;
bool     g_edge_pending    = false;   // true between debounced press and consume
}  // namespace

void keyInit() {
  pinMode(kPin, INPUT_PULLUP);
  g_last_raw = digitalRead(kPin);
  g_stable_value = g_last_raw;
  g_stable_since_ms = millis();
}

bool keyPressedSinceLastCall() {
  uint32_t now = millis();
  if (now - g_last_sample_ms >= kSamplePeriodMs) {
    g_last_sample_ms = now;
    int raw = digitalRead(kPin);
    if (raw != g_last_raw) {
      g_last_raw = raw;
      g_stable_since_ms = now;     // raw changed → restart stability timer
    } else if (raw != g_stable_value && (now - g_stable_since_ms) >= kDebounceMs) {
      // raw has been stable at a new value for >= debounce window
      g_stable_value = raw;
      if (raw == LOW) g_edge_pending = true;   // press edge HIGH→LOW
    }
  }
  if (g_edge_pending) {
    g_edge_pending = false;
    return true;
  }
  return false;
}
