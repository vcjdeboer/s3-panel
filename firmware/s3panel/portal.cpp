#include "portal.h"

#include <ArduinoJson.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>
#include <string.h>

#include "badge_ui.h"
#include "config.h"
#include "net.h"
#include "profile.h"
#include "util.h"

static WebServer *server = nullptr;
static DNSServer dns;
static PortalPhase phase = PORTAL_IDLE;
static char nets[16][33];
static int netCount = 0;
static bool scanning = false;  // a background scan is filling nets[]
static char wantSsid[33], wantPass[65], wantUser[40];
static char message[128];
static bool pending = false;
static char recent[8][48];  // the last requests: "GET /generate_204 host"
static int recentNext = 0;
static unsigned long requests = 0;
static bool drawing = true;  // off while swamp holds the screen

void portalSetDrawing(bool on) { drawing = on; }

static void note() {
  requests++;
  snprintf(recent[recentNext], sizeof recent[0], "%s %s %s",
           server->method() == HTTP_POST ? "POST" : "GET", server->uri().c_str(),
           server->hostHeader().c_str());
  recentNext = (recentNext + 1) % 8;
}

static const char PAGE_HEAD[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>swamp badge</title><style>"
    "body{font-family:sans-serif;background:#000;color:#eee;max-width:420px;margin:24px auto;padding:0 16px}"
    "h1{color:#4bd}label{display:block;margin-top:16px}"
    "input,select{width:100%;font-size:18px;padding:8px;box-sizing:border-box}"
    "button{margin-top:24px;width:100%;font-size:20px;padding:12px;background:#4bd;border:0}"
    ".err{color:#f7a}a{color:#4bd}</style></head><body><h1>swamp badge</h1>";

static void sendForm() {
  note();
  char esc[200];
  String h = PAGE_HEAD;
  if (message[0]) {
    htmlEscape(message, esc, sizeof esc);
    h += "<p class=err>";
    h += esc;
    h += "</p>";
  }
  if (scanning) h += "<p>Looking for networks... <a href=/>refresh</a> in a few seconds.</p>";
  h += "<form method=post action=/save><label>Wi-Fi network<select name=ssid>";
  for (int i = 0; i < netCount; i++) {
    htmlEscape(nets[i], esc, sizeof esc);
    h += "<option value=\"";  // explicit value: option text gets whitespace-trimmed
    h += esc;
    h += "\"";
    if (!strcmp(nets[i], wantSsid)) h += " selected";
    h += ">";
    h += esc;
    h += "</option>";
  }
  bool listed = false;
  for (int i = 0; i < netCount; i++) listed = listed || !strcmp(nets[i], wantSsid);
  h += "<option value=''>other...</option></select></label>";
  h += "<label>other network name (wins if filled in)<input name=other maxlength=32 value=\"";
  if (!listed) {
    htmlEscape(wantSsid, esc, sizeof esc);
    h += esc;
  }
  h += "\"></label>";
  h += "<label>Wi-Fi password<input name=pass type=password maxlength=63></label>";
  htmlEscape(wantUser, esc, sizeof esc);
  h += "<label>swamp username<input name=user maxlength=39 autocapitalize=off "
       "autocorrect=off spellcheck=false value=\"";
  h += esc;
  h += "\"></label><button>Connect</button></form>"
       "<p><a href=/rescan>rescan networks</a></p></body></html>";
  server->send(200, "text/html", h);
}

static void sendChecking() {
  String h = PAGE_HEAD;
  h += "<meta http-equiv=refresh content='3;url=/result'>"
       "<p>Checking... watch the panel. This page refreshes by itself.</p></body></html>";
  server->send(200, "text/html", h);
}

static void sendDone() {
  String h = PAGE_HEAD;
  h += "<p>Done. You can leave this network.</p></body></html>";
  server->send(200, "text/html", h);
}

static void redirectHome() {
  note();
  server->sendHeader("Location", "http://192.168.4.1/", true);
  server->send(302, "text/plain", "");
}

static void handleSave() {
  note();
  String selected = server->arg("ssid"), other = server->arg("other");
  String ssid = pickSsid(selected.c_str(), other.c_str());
  String pass = server->arg("pass");
  String user = server->arg("user");
  user.trim();
  strlcpy(wantSsid, ssid.c_str(), sizeof wantSsid);
  strlcpy(wantUser, user.c_str(), sizeof wantUser);
  if (!ssid.length() || ssid.length() > 32) {
    strlcpy(message, "pick a Wi-Fi network", sizeof message);
    sendForm();
    return;
  }
  if (pass.length() > 63 || (pass.length() > 0 && pass.length() < 8)) {
    strlcpy(message, "a Wi-Fi password is 8 to 63 characters", sizeof message);
    sendForm();
    return;
  }
  if (!validUsername(wantUser)) {
    strlcpy(message, "a swamp username is letters, digits, . _ or - (up to 39)", sizeof message);
    sendForm();
    return;
  }
  strlcpy(wantPass, pass.c_str(), sizeof wantPass);
  message[0] = '\0';
  pending = true;
  phase = PORTAL_CHECKING;
  sendChecking();
}

static void handleResult() {
  note();
  if (phase == PORTAL_CHECKING) sendChecking();
  else if (phase == PORTAL_DONE) sendDone();
  else sendForm();
}

static void handleRescan() {
  note();
  netScanStart();
  scanning = true;
  redirectHome();
}

static void runCheck() {
  pending = false;
  if (drawing) uiSetupChecking(wantSsid);
  // Messages leave the network name out: `setup status` reports them over USB.
  if (!netJoin(wantSsid, wantPass, 20000)) {
    const char *why = joinReasonText(netLastReason());
    snprintf(message, sizeof message, "couldn't join that network: %s", why);
    if (drawing) uiSetupError("couldn't join", why);
    memset(wantPass, 0, sizeof wantPass);
    phase = PORTAL_ERROR;
    return;
  }
  PanelConfig c;
  configLoad(c);
  profileWaitIdle(30000);
  static Profile p;
  memset(&p, 0, sizeof p);
  int code = 0;
  FetchResult r = profileFetch(c.api, wantUser, p, code);
  if (r != FETCH_OK) {
    if (r == FETCH_NOT_FOUND) {
      snprintf(message, sizeof message, "no swamp user called %s", wantUser);
      if (drawing) uiSetupError("no swamp user", wantUser);
    } else {
      snprintf(message, sizeof message, "joined the network but couldn't reach swamp-club.com");
      if (drawing) uiSetupError("no swamp-club via", wantSsid);
    }
    WiFi.disconnect();  // station only; the hotspot stays
    memset(wantPass, 0, sizeof wantPass);
    phase = PORTAL_ERROR;
    return;
  }
  configSaveWifi(wantSsid, wantPass);
  configSaveProfile(wantUser);
  memset(wantPass, 0, sizeof wantPass);
  profileBegin(c.api, wantUser);
  profileAdopt(p);
  phase = PORTAL_DONE;
}

void portalBegin() {
  netCount = 0;
  netScanStart();  // in the background: entering setup must not block the loop
  scanning = true;
  message[0] = '\0';
  phase = PORTAL_IDLE;
  pending = false;
  server = new WebServer(80);
  server->on("/", HTTP_GET, sendForm);
  server->on("/save", HTTP_POST, handleSave);
  server->on("/result", HTTP_GET, handleResult);
  server->on("/rescan", HTTP_GET, handleRescan);
  server->onNotFound(redirectHome);  // captive-portal probes land here
  server->begin();
  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53, "*", WiFi.softAPIP());
}

void portalLoop() {
  if (!server) return;
  dns.processNextRequest();
  if (scanning) {
    int n = netScanCollect(nets, 16);
    if (n >= 0) {
      netCount = n;
      scanning = false;
    }
  }
  server->handleClient();
  if (pending) runCheck();
}

void portalEnd() {
  if (!server) return;
  server->stop();
  delete server;
  server = nullptr;
  dns.stop();
  phase = PORTAL_IDLE;
}

PortalPhase portalPhase() { return phase; }

void portalStatusJson(Print &out) {
  static const char *names[] = {"idle", "checking", "error", "done"};
  JsonDocument d;
  d["ok"] = true;
  d["running"] = server != nullptr;
  d["phase"] = names[phase];
  d["message"] = message;
  d["networks"] = netCount;
  d["scanning"] = scanning;
  d["clients"] = netApClients();
  d["requests"] = requests;
  JsonArray r = d["recent"].to<JsonArray>();
  for (int i = 0; i < 8; i++) {
    const char *e = recent[(recentNext + i) % 8];
    if (e[0]) r.add(e);
  }
  serializeJson(d, out);
  out.println();
}
