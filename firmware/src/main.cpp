// firmware/src/main.cpp
#include <Arduino.h>
#include <WiFi.h>
#include "secrets.h"

static void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PWD);
  Serial.printf("[wifi] connecting to %s ", WIFI_SSID);
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
  }
  Serial.printf("\n[wifi] OK ip=%s\n", WiFi.localIP().toString().c_str());
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("[boot] esp32-ai-usage-display v0");
  connectWifi();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[wifi] dropped, reconnecting");
    connectWifi();
  }
  delay(5000);
}
