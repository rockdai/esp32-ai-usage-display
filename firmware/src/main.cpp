#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "secrets.h"
#include "state.h"
#include "api.h"
#include "display.h"
#include "render.h"

static WebServer server(80);
static UsageData g_state;
static SemaphoreHandle_t g_mutex;
static volatile bool g_dirty = true;
static volatile uint32_t g_last_post_ms = 0;

static void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PWD);
  Serial.printf("[wifi] connecting to %s ", WIFI_SSID);
  while (WiFi.status() != WL_CONNECTED) { Serial.print("."); delay(500); }
  Serial.printf("\n[wifi] OK ip=%s\n", WiFi.localIP().toString().c_str());
}

static void startMdns() {
  if (!MDNS.begin("ai-desktop-buddy")) { Serial.println("[mdns] FAILED"); return; }
  MDNS.addService("http", "tcp", 80);
  Serial.println("[mdns] ai-desktop-buddy.local advertised");
}

static void handleData() {
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "no body"); return; }
  UsageData parsed;
  if (!parseUsageJson(server.arg("plain").c_str(), parsed)) {
    server.send(400, "text/plain", "bad json");
    return;
  }
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  g_state = parsed;
  g_dirty = true;
  g_last_post_ms = millis();
  xSemaphoreGive(g_mutex);
  Serial.printf("[api] state updated tok_5h=%llu tok_w=%llu\n",
                (unsigned long long)parsed.tok_5h, (unsigned long long)parsed.tok_weekly);
  server.send(200, "text/plain", "ok");
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("[boot] esp32-ai-desktop-buddy v0");
  displayInit();
  renderInit();
  g_mutex = xSemaphoreCreateMutex();
  connectWifi();
  startMdns();
  server.on("/data", HTTP_POST, handleData);
  server.begin();
  Serial.println("[http] server on :80");
}

void loop() {
  server.handleClient();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[wifi] dropped, reconnecting");
    connectWifi();
    startMdns();
  }

  static uint32_t last_render = 0;
  if (millis() - last_render >= 1000) {
    last_render = millis();
    uint32_t age = millis() - g_last_post_ms;
    bool wifi_ok = WiFi.status() == WL_CONNECTED;
    bool stale = age > 300000UL;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    UsageData snap = g_state;
    xSemaphoreGive(g_mutex);
    renderTick(snap, stale, wifi_ok, age);
  }

  delay(10);
}
