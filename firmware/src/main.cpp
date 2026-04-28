// firmware/src/main.cpp
#include <Arduino.h>

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("[boot] esp32-ai-usage-display v0");
}

void loop() {
  Serial.println("[heartbeat]");
  delay(5000);
}
