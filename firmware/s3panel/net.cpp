#include "net.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_mac.h>
#include <time.h>

#include "util.h"

static bool sntpStarted = false;
static volatile int lastReason = 0;
static volatile bool joining = false;  // count only the join's own failures

void netInit() {
  // Arduino otherwise also stores credentials in the IDF's own NVS, where
  // `config forget` would not reach them.
  WiFi.persistent(false);
  WiFi.onEvent(
      [](arduino_event_id_t, arduino_event_info_t info) {
        int r = info.wifi_sta_disconnected.reason;
        // The retry loop leaves between attempts (STA_LEAVING, ASSOC_LEAVE);
        // keep the reason the attempt itself failed with.
        if (joining && r != 36 && r != 8) lastReason = r;
      },
      ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
}

int netLastReason() { return lastReason; }

void netBegin(const char *ssid, const char *pass) {
  if (!ssid[0]) return;
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, pass);
}

bool netConnected() { return WiFi.status() == WL_CONNECTED; }

bool netJoin(const char *ssid, const char *pass, uint32_t timeoutMs) {
  bool ap = (WiFi.getMode() & WIFI_AP) != 0;
  WiFi.mode(ap ? WIFI_AP_STA : WIFI_STA);
  if (WiFi.status() == WL_CONNECTED) {
    // Leave the current network, and let that finish, before joining another.
    WiFi.disconnect();
    unsigned long t = millis();
    while (WiFi.status() == WL_CONNECTED && millis() - t < 2000) delay(20);
    delay(200);
  }
  lastReason = 0;
  joining = true;
  WiFi.begin(ssid, pass);
  unsigned long t0 = millis();
  while (millis() - t0 < timeoutMs) {
    if (WiFi.status() == WL_CONNECTED) {
      joining = false;
      return true;
    }
    delay(100);
  }
  joining = false;  // our own disconnect below is not a reason
  WiFi.disconnect();
  return false;
}

void netApStart(const char *ssid, const char *pass) {
  WiFi.mode(WIFI_AP_STA);  // STA too, so the portal can scan and join
  WiFi.softAP(ssid, pass);
}

void netApStop() {
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
}

int netApClients() { return WiFi.softAPgetStationNum(); }

static int collect(int n, char names[][33], int max);

int netScan(char names[][33], int max) { return collect(WiFi.scanNetworks(), names, max); }

void netScanStart() { WiFi.scanNetworks(true); }

int netScanCollect(char names[][33], int max) {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return -1;
  if (n < 0) return 0;  // failed or not started
  return collect(n, names, max);
}

static int collect(int n, char names[][33], int max) {
  int count = 0;
  while (count < max) {
    int best = -1;
    for (int i = 0; i < n; i++) {
      String s = WiFi.SSID(i);
      if (!s.length()) continue;
      bool seen = false;
      for (int j = 0; j < count; j++) {
        if (s == names[j]) { seen = true; break; }
      }
      if (seen) continue;
      if (best < 0 || WiFi.RSSI(i) > WiFi.RSSI(best)) best = i;
    }
    if (best < 0) break;
    strlcpy(names[count++], WiFi.SSID(best).c_str(), 33);
  }
  WiFi.scanDelete();
  return count;
}

void netLoop() {
  if (!sntpStarted && netConnected()) {
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    sntpStarted = true;
  }
}

int64_t netNow() {
  time_t t = time(nullptr);
  return t >= CLOCK_VALID_AFTER ? (int64_t)t : 0;
}

void netApName(char *out, size_t cap) {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
  snprintf(out, cap, "swamp-%02x%02x", mac[4], mac[5]);
}

void netStatusJson(Print &out, const char *state) {
  JsonDocument d;
  bool c = netConnected();
  d["ok"] = true;
  d["state"] = state;
  d["connected"] = c;
  d["ip"] = c ? WiFi.localIP().toString() : String("");
  d["rssi"] = c ? WiFi.RSSI() : 0;
  d["mac"] = WiFi.macAddress();
  serializeJson(d, out);
  out.println();
}
