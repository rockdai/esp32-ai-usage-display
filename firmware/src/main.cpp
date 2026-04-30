#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "secrets.h"
#include "state.h"
#include "api.h"
#include "attention.h"
#include "display.h"
#include "render.h"
#include "key.h"

static WebServer server(80);
static UsageData g_state;
static AttentionState g_attention;
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

static void handleAttention() {
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "no body"); return; }
  AttentionState parsed;
  if (!parseAttentionJson(server.arg("plain").c_str(), parsed)) {
    server.send(400, "text/plain", "bad json");
    return;
  }
  parsed.since_ms = millis();
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  g_attention = parsed;
  g_dirty = true;
  xSemaphoreGive(g_mutex);
  Serial.printf("[api] attention kind=%d cwd=%s\n", (int)parsed.kind, parsed.cwd);
  server.send(200, "text/plain", "ok");
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("[boot] esp32-ai-desktop-buddy v0");
  displayInit();
  renderInit();
  g_mutex = xSemaphoreCreateMutex();
  keyInit();
  connectWifi();
  startMdns();
  server.on("/data", HTTP_POST, handleData);
  server.on("/attention", HTTP_POST, handleAttention);
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

  if (keyPressedSinceLastCall()) {
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_attention.kind = ATTN_IDLE;
    g_attention.since_ms = millis();
    g_attention.cwd[0] = '\0';
    g_dirty = true;
    xSemaphoreGive(g_mutex);
    Serial.println("[key] dismissed → IDLE");
  }

  static uint32_t last_render = 0;
  if (millis() - last_render >= 1000) {
    last_render = millis();
    bool wifi_ok = WiFi.status() == WL_CONNECTED;
    uint32_t age = millis() - g_last_post_ms;
    bool stale = age > 300000UL;

    xSemaphoreTake(g_mutex, portMAX_DELAY);
    UsageData      snap_u = g_state;
    AttentionState snap_a = g_attention;
    if (attentionTick(g_attention, millis())) {
      snap_a = g_attention;       // pick up the IDLE transition
      g_dirty = true;
    }
    xSemaphoreGive(g_mutex);

    renderTick(snap_u, snap_a, stale, wifi_ok, age);
  }

  delay(10);
}
