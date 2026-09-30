// s3panel -- firmware for the diymore / Guition JC3248W535:
// an ESP32-S3 (N16R8) with a 3.5" 320x480 AXS15231B panel on a QSPI bus.
//
// The host owns the logic. This firmware exposes a few high-level commands over
// USB serial, one command per line in, one JSON object per line out, so the
// host never has to push pixels. Driven from swamp by @vcjdeboer/s3-panel.
//
//   ping              -> {"ok":true,"fw":"s3panel 0.12"}
//   status            -> {"ok":true,"display":bool,"w":320,"h":480,"psram":N,
//                          "backlight":bool,"touch":bool}
//   text <msg>        -> {"ok":true,"lines":N}    ('|' splits lines, drawn top-down)
//   clear             -> {"ok":true}
//   fill <name>       -> {"ok":true,"color":"<name>"}
//                        names: black white red green blue yellow cyan magenta
//   backlight on|off  -> {"ok":true,"backlight":bool}
//   touch             -> {"ok":true,"points":N,"x":X,"y":Y,"gesture":N}
//   touchstate        -> {"ok":true,"count":N,"x":X,"y":Y,"t":ms}
//                        latched: count increments on every touch, cleared by
//                        touchclear. x/y/t are the last touch coordinates and
//                        timestamp. count=0 means no touch since last clear.
//   touchclear        -> {"ok":true,"cleared":N}  resets count to 0
//   waittouch [ms]    -> blocks until a touch, returns {"ok":true,...} or
//                        {"ok":true,"points":0,"timeout":true} on expiry, or
//                        {"ok":true,"points":0,"aborted":true} on serial input
//   simtouch X Y      -> fake a touch at (X,Y): sets latch as if touched
//   touchgame         -> {"ok":true,"game":"started"}  tap counter game
//   events on|off     -> {"ok":true,"events":bool}  unsolicited tap events
//                        when on, each tap emits: {"event":"tap","n":N,"x":X,
//                        "y":Y,"t":ms}  (no qid, capture with listen)
//   logo              -> {"ok":true}   draws logo, touch it to explode
//   screen clear      -> {"ok":true}   empties the pending layout
//   screen add <json> -> {"ok":true,"index":N}  element: label, button, gap;
//                        a button with an "id" becomes a touch zone
//   screen show       -> {"ok":true,"elements":N,"zones":N}  renders the layout
//                        below a mini logo
//   screen wait [ms]  -> blocks until a zone is tapped:
//                        {"ok":true,"id":"...","x":X,"y":Y}, or
//                        {"ok":true,"timeout":true} on expiry (default 300000),
//                        or {"ok":true,"aborted":true} when a serial line
//                        arrives first (the line is consumed, not run).
//                        A tapped "approve"/"reject" zone flashes APPROVED /
//                        REJECTED; any other zone is outlined in white.
//   anything else     -> {"ok":false,"error":"..."}
//
// Panel pin map, QSPI (verified for this board): CS 45, SCK 47, D0 21, D1 48,
// D2 40, D3 39. Backlight on GPIO 1. The 320x480x16bpp canvas needs PSRAM, so
// build with PSRAM=opi.
//
// Touch is AXS15231B over I2C at 0x3B (SDA 4, SCL 8, INT 11, RST 12).
// The display MUST be initialized before touch — AXS15231B is a unified chip
// and the QSPI display init configures the entire controller including touch.

#include <Arduino_GFX_Library.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_random.h>

#include "badge_ui.h"
#include "config.h"
#include "device_state.h"
#include "net.h"
#include "portal.h"
#include "profile.h"
#include "panel.h"
#include "util.h"

#define FW "s3panel 0.12.2"
#define TOUCH_ADDR 0x3B
#define TOUCH_SDA 4
#define TOUCH_SCL 8
#define TOUCH_INT 11
#define TOUCH_RST 12
#define TOUCH_I2C_CLOCK 400000
#define PIN_BL 1

#ifndef PANEL_INIT_TYPE
#define PANEL_INIT_TYPE 1
#endif
#if PANEL_INIT_TYPE == 2
#define PANEL_INIT axs15231b_320480_type2_init_operations
#else
#define PANEL_INIT axs15231b_320480_type1_init_operations
#endif

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    45 /* CS */, 47 /* SCK */, 21 /* D0 */, 48 /* D1 */, 40 /* D2 */, 39 /* D3 */);
Arduino_GFX *panel = new Arduino_AXS15231B(
    bus, GFX_NOT_DEFINED /* RST */, 0 /* rotation */, false /* IPS */, SCREEN_W, SCREEN_H,
    0, 0, 0, 0, PANEL_INIT, sizeof(PANEL_INIT));
Arduino_Canvas *gfx = new Arduino_Canvas(SCREEN_W, SCREEN_H, panel, 0, 0, 0);

bool displayOk = false;
static bool touchOk = false;
static bool backlightOn = false;
static bool logoMode = false;
static bool gameMode = false;
static bool eventsOn = false;
static String buf;

// Touch latch: accumulates every touch so the host never misses one.
static volatile uint32_t touchCount = 0;
static int touchLastX = 0;
static int touchLastY = 0;
static unsigned long touchLastMs = 0;

static void latchTouch(int x, int y) {
  touchLastX = x;
  touchLastY = y;
  touchLastMs = millis();
  touchCount++;
}

// ── device state ─────────────────────────────────────────────────────────────

static DevState devState = ST_CONNECTING;
static PanelConfig cfg;
static void showState();
static void enterSetup();  // defined with the setup state
static void leaveSetup();  // defined with the setup state
static bool inSetup();     // setup, or swamp holding the screen during setup
static char apSsid[16], apPass[9];
static bool setupShowingOpen = false;
static unsigned long setupDoneMs = 0;

static const char *stateName(DevState s) {
  switch (s) {
    case ST_SETUP: return "setup";
    case ST_CONNECTING: return "connecting";
    case ST_BADGE: return "badge";
    default: return "host";
  }
}

static void replyError(const char *e) {
  JsonDocument d;
  d["ok"] = false;
  d["error"] = e;
  serializeJson(d, Serial);
  Serial.println();
}

static bool validPass(const char *p) {
  size_t n = strlen(p);
  return n == 0 || (n >= 8 && n <= 63);
}

// config set|show|forget. Replies name keys, never values.
static void handleConfig(const String &arg) {
  String sub = arg, rest = "";
  int sp = arg.indexOf(' ');
  if (sp >= 0) {
    sub = arg.substring(0, sp);
    rest = arg.substring(sp + 1);
  }
  if (sub == "show") {
    JsonDocument d;
    d["ok"] = true;
    d["ssidSet"] = cfg.ssid[0] != 0;
    d["passSet"] = cfg.pass[0] != 0;
    d["profile"] = cfg.profile;
    d["api"] = cfg.api;
    serializeJson(d, Serial);
    Serial.println();
    return;
  }
  if (sub == "forget") {
    configForget();
    // Also erase the IDF's own stored network, which other firmware may have left.
    WiFi.disconnect(true, true);
    configLoad(cfg);
    profileBegin(cfg.api, "");
    // Answer first: entering setup starts the hotspot and scans (2-4 s).
    Serial.println("{\"ok\":true,\"forgotten\":true}");
    Serial.flush();
    portalEnd();
    enterSetup();
    return;
  }
  if (sub != "set") {
    replyError("config set|show|forget");
    return;
  }
  JsonDocument in;
  if (deserializeJson(in, rest)) {
    replyError("invalid");
    return;
  }
  const char *ssid = in["ssid"];
  const char *pass = in["pass"] | "";
  const char *profile = in["profile"];
  const char *api = in["api"];
  if ((ssid && (!ssid[0] || strlen(ssid) > 32)) || !validPass(pass) ||
      (profile && !validUsername(profile)) ||
      (api && (strlen(api) >= sizeof cfg.api || (api[0] && strncmp(api, "https://", 8) != 0)))) {
    replyError("invalid");
    return;
  }
  bool joined = false;
  if (ssid) {
    if (!netJoin(ssid, pass, 20000)) {
      char why[48];
      snprintf(why, sizeof why, "join: %s", joinReasonText(netLastReason()));
      netBegin(cfg.ssid, cfg.pass);  // back to the saved network, if any
      replyError(why);
      return;
    }
    joined = true;
  }
  static Profile fetched;
  if (profile) {
    if (!netConnected()) {
      replyError("unreachable");
      return;
    }
    profileWaitIdle(30000);
    memset(&fetched, 0, sizeof fetched);
    int code = 0;
    FetchResult r = profileFetch(api && api[0] ? api : cfg.api, profile, fetched, code);
    if (r != FETCH_OK) {
      if (joined) netBegin(cfg.ssid, cfg.pass);
      char why[48];
      if (r == FETCH_NOT_FOUND) snprintf(why, sizeof why, "not found");
      else if (r == FETCH_HTTP) snprintf(why, sizeof why, "unreachable: http %d", code);
      else if (r == FETCH_PARSE) snprintf(why, sizeof why, "unreachable: parse %s", profileParseNote());
      else snprintf(why, sizeof why, "unreachable: connect %d", code);
      replyError(why);
      return;
    }
  }
  JsonDocument d;
  d["ok"] = true;
  JsonArray stored = d["stored"].to<JsonArray>();
  if (ssid) {
    configSaveWifi(ssid, pass);
    stored.add("ssid");
    stored.add("pass");
  }
  if (api) {
    configSaveApi(api);
    stored.add("api");
  }
  if (profile) {
    configSaveProfile(profile);
    stored.add("profile");
  }
  configLoad(cfg);
  if (profile || api) {
    profileBegin(cfg.api, cfg.profile);
    if (profile) profileAdopt(fetched);
  }
  d["joined"] = joined || netConnected();
  if (inSetup() && configComplete(cfg)) leaveSetup();
  serializeJson(d, Serial);
  Serial.println();
}


static DevState beforeHost = ST_CONNECTING;
static unsigned long lastHostMs = 0;
static const unsigned long HOST_IDLE_MS = 5UL * 60 * 1000;

static bool drawsOrWaits(const String &cmd) {
  return cmd == "text" || cmd == "clear" || cmd == "fill" || cmd == "logo" ||
         cmd == "backlight" || cmd == "touchgame" || cmd == "waittouch" || cmd == "screen";
}

// Redraw whatever the current non-host state shows.
static void showState() {
  if (!displayOk) return;
  if (devState == ST_SETUP) {
    if (setupShowingOpen) uiSetupOpen();
    else uiSetupJoin(apSsid, apPass);
    return;
  }
  if (devState == ST_CONNECTING) uiConnecting();
  else if (devState == ST_BADGE) uiBadgeEnter();
}

static void enterHost() {
  if (devState != ST_HOST) {
    beforeHost = devState;
    devState = ST_HOST;
    setBacklightPct(100);  // a later `backlight off` still wins
  }
  lastHostMs = millis();
}

static void leaveHost() {
  if (devState != ST_HOST) return;
  devState = beforeHost;
  logoMode = false;
  gameMode = false;
  showState();
}

static void enterSetup() {
  devState = ST_SETUP;
  setupShowingOpen = false;
  setupDoneMs = 0;
  netApName(esp_random(), apSsid, sizeof apSsid);
  randomPassword(esp_random, apPass, 8);
  netApStart(apSsid, apPass);
  portalBegin();
  if (displayOk) uiSetupJoin(apSsid, apPass);
}

static bool inSetup() {
  return devState == ST_SETUP || (devState == ST_HOST && beforeHost == ST_SETUP);
}

// Finish setup. Under swamp, only the state to hand back to changes: the screen
// stays swamp's.
static void leaveSetup() {
  portalEnd();
  netApStop();
  configLoad(cfg);
  DevState next = (netConnected() || profileHave()) ? ST_BADGE : ST_CONNECTING;
  if (devState == ST_HOST) {
    beforeHost = next;
    return;
  }
  devState = next;
  showState();
}

// Keep the phone's setup page answering; called from the loop and from the
// blocking touch waits, so a swamp command never freezes it.
static void setupService() {
  if (!inSetup()) return;
  portalSetDrawing(devState == ST_SETUP);
  portalLoop();
}

// ── helpers ──────────────────────────────────────────────────────────────────

#define BL_FREQ 5000
#define BL_BITS 8

void setBacklightPct(uint8_t pct) {
  if (pct > 100) pct = 100;
  ledcWrite(PIN_BL, (uint32_t)pct * 255 / 100);
  backlightOn = pct > 0;
}

static void setBacklight(bool on) { setBacklightPct(on ? 100 : 0); }

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

static uint16_t colorByName(const String &name, bool *found) {
  *found = true;
  if (name == "black") return RGB565_BLACK;
  if (name == "white") return RGB565_WHITE;
  if (name == "red") return RGB565_RED;
  if (name == "green") return RGB565_GREEN;
  if (name == "blue") return RGB565_BLUE;
  if (name == "yellow") return RGB565_YELLOW;
  if (name == "cyan") return RGB565_CYAN;
  if (name == "magenta") return RGB565_MAGENTA;
  if (name == "pink") return 0xD98F;
  *found = false;
  return RGB565_BLACK;
}

static bool readTouch(int *x, int *y, int *gesture) {
  static const uint8_t readCmd[11] = {
    0xB5, 0xAB, 0xA5, 0x5A, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00
  };
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(readCmd, 11);
  Wire.endTransmission();
  uint8_t data[8] = {};
  uint8_t n = Wire.requestFrom((uint8_t)TOUCH_ADDR, (uint8_t)8);
  for (uint8_t i = 0; i < n && Wire.available(); i++) data[i] = Wire.read();
  int points = data[1];
  if (data[0] == 0 && points > 0 && points <= 5) {
    *x = ((data[2] & 0x0F) << 8) | data[3];
    *y = ((data[4] & 0x0F) << 8) | data[5];
    *gesture = data[0];
    return true;
  }
  return false;
}

static int drawText(const String &msg) {
  if (!displayOk) return 0;
  gfx->fillScreen(RGB565_BLACK);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(3);
  int line = 0;
  int start = 0;
  while (start <= msg.length() && line < 12) {
    int bar = msg.indexOf('|', start);
    String part = (bar < 0) ? msg.substring(start) : msg.substring(start, bar);
    gfx->setCursor(10, 20 + line * 34);
    gfx->println(part);
    line++;
    if (bar < 0) break;
    start = bar + 1;
  }
  gfx->flush();
  return line;
}

// ── logo ─────────────────────────────────────────────────────────────────────

#define LOGO_BG     0x0000

void drawLogo() {
  randomSeed(42);
  gfx->fillScreen(LOGO_BG);

  // background drips (behind text)
  for (int i = 0; i < 8; i++) {
    int dx = 30 + random(0, 260);
    int len = 60 + random(0, 200);
    uint16_t col = (i % 2 == 0) ? CYAN_DIM : PINK_DIM;
    gfx->drawFastVLine(dx, 0, len, col);
  }

  // "SWAMP" in cyan with drips
  int swampX = (SCREEN_W - 5 * 36) / 2;
  gfx->setTextColor(CYAN_TXT);
  gfx->setTextSize(6);
  gfx->setCursor(swampX, 15);
  gfx->print("SWAMP");
  for (int i = 0; i < 14; i++) {
    int dx = swampX + random(0, 180);
    int len = 10 + random(0, 45);
    int thick = random(0, 3) == 0 ? 2 : 1;
    for (int t = 0; t < thick; t++)
      gfx->drawFastVLine(dx + t, 63, len, CYAN_TXT);
    if (random(0, 3) == 0)
      gfx->fillCircle(dx, 63 + len + 1, 2, CYAN_TXT);
  }

  // "CLUB" in pink with drips
  int clubX = (SCREEN_W - 4 * 36) / 2;
  gfx->setTextColor(PINK_TXT);
  gfx->setTextSize(6);
  gfx->setCursor(clubX, 85);
  gfx->print("CLUB");
  for (int i = 0; i < 12; i++) {
    int dx = clubX + random(0, 144);
    int len = 10 + random(0, 50);
    int thick = random(0, 3) == 0 ? 2 : 1;
    for (int t = 0; t < thick; t++)
      gfx->drawFastVLine(dx + t, 133, len, PINK_TXT);
    if (random(0, 3) == 0)
      gfx->fillCircle(dx, 133 + len + 1, 2, PINK_TXT);
  }

  // creature emerging from swamp
  int waterY = 340;

  // smooth body (overlapping circles — bigger and wider)
  gfx->fillCircle(160, 278, 24, BODY_DARK);
  gfx->fillCircle(160, 312, 36, BODY_DARK);
  gfx->fillRect(136, 278, 48, 38, BODY_DARK);
  gfx->fillCircle(108, 330, 22, BODY_DARK);
  gfx->fillCircle(212, 330, 22, BODY_DARK);

  // glow outline (above water only)
  for (int a = -170; a <= -10; a += 6) {
    float r1 = a * 0.01745f, r2 = (a + 6) * 0.01745f;
    gfx->drawLine(160 + (int)(cosf(r1) * 25), 278 + (int)(sinf(r1) * 25),
                  160 + (int)(cosf(r2) * 25), 278 + (int)(sinf(r2) * 25), BODY_GLOW);
  }
  gfx->drawLine(136, 290, 86, waterY, BODY_GLOW);
  gfx->drawLine(184, 290, 234, waterY, BODY_GLOW);

  // yellow eyes
  gfx->fillTriangle(141, 288, 148, 282, 155, 288, EYE_YELLOW);
  gfx->fillTriangle(141, 288, 148, 294, 155, 288, EYE_YELLOW);
  gfx->fillTriangle(165, 288, 172, 282, 179, 288, EYE_YELLOW);
  gfx->fillTriangle(165, 288, 172, 294, 179, 288, EYE_YELLOW);

  // water surface (cuts across creature's lower body)
  gfx->fillRect(0, waterY, SCREEN_W, SCREEN_H - waterY, LOGO_BG);
  gfx->drawFastHLine(0, waterY, SCREEN_W, BODY_GLOW);
  gfx->drawFastHLine(0, waterY + 1, SCREEN_W, CYAN_DIM);
  // ripples spreading from creature
  gfx->drawFastHLine(130, waterY + 5, 60, CYAN_DIM);
  gfx->drawFastHLine(110, waterY + 10, 100, CYAN_DIM);
  gfx->drawFastHLine(85, waterY + 17, 150, CYAN_DIM);
  gfx->drawFastHLine(60, waterY + 26, 200, CYAN_DIM);

  // water reflections
  randomSeed(99);
  for (int i = 0; i < 20; i++) {
    int ry = waterY + 35 + random(0, 55);
    int rx = random(20, 240);
    int rw = 15 + random(0, 65);
    uint16_t col = (random(0, 2) == 0) ? CYAN_DIM : PINK_DIM;
    gfx->drawFastHLine(rx, ry, rw, col);
  }

  // SC hexagon badge
  const int hx[] = {160, 177, 177, 160, 143, 143};
  const int hy[] = {428, 438, 458, 468, 458, 438};
  for (int i = 0; i < 6; i++)
    gfx->drawLine(hx[i], hy[i], hx[(i + 1) % 6], hy[(i + 1) % 6], PINK_TXT);
  gfx->setTextColor(PINK_TXT);
  gfx->setTextSize(2);
  gfx->setCursor(149, 442);
  gfx->print("SC");

  // subtitle
  gfx->setTextSize(1);
  gfx->setTextColor(CYAN_DIM);
  gfx->setCursor(128, SCREEN_H - 8);
  gfx->print("touch me");

  gfx->flush();
}

// ── explosion ────────────────────────────────────────────────────────────────

#define NUM_PARTICLES 50

struct Particle {
  float x, y, vx, vy;
  uint16_t color;
  int size;
  bool alive;
};

static Particle particles[NUM_PARTICLES];

static void spawnExplosion(int cx, int cy) {
  const uint16_t colors[] = {
    CYAN_TXT, PINK_TXT, RGB565_WHITE, EYE_YELLOW,
    BODY_GLOW, 0xFC00, 0xFA00,
  };
  for (int i = 0; i < NUM_PARTICLES; i++) {
    float angle = random(0, 628) / 100.0f;
    float speed = 2.0f + random(0, 800) / 100.0f;
    particles[i].x = cx;
    particles[i].y = cy;
    particles[i].vx = cosf(angle) * speed;
    particles[i].vy = sinf(angle) * speed;
    particles[i].color = colors[random(0, 7)];
    particles[i].size = 2 + random(0, 5);
    particles[i].alive = true;
  }
}

static void runExplosion() {
  // flash
  gfx->fillScreen(RGB565_WHITE);
  gfx->flush();
  delay(40);

  for (int frame = 0; frame < 55; frame++) {
    gfx->fillScreen(LOGO_BG);

    int alive = 0;
    for (int i = 0; i < NUM_PARTICLES; i++) {
      Particle &p = particles[i];
      if (!p.alive) continue;

      p.x += p.vx;
      p.y += p.vy;
      p.vy += 0.15f; // gravity
      p.vx *= 0.98f; // drag

      // shrink over time
      if (frame > 30 && p.size > 1) p.size--;
      if (frame > 40 && random(0, 3) == 0) p.size--;
      if (p.size <= 0 || p.x < -20 || p.x > SCREEN_W + 20 ||
          p.y > SCREEN_H + 20) {
        p.alive = false;
        continue;
      }

      gfx->fillCircle((int)p.x, (int)p.y, p.size, p.color);
      alive++;
    }

    // sparks: tiny bright dots trailing some particles
    if (frame < 30) {
      for (int i = 0; i < NUM_PARTICLES; i += 3) {
        if (!particles[i].alive) continue;
        int sx = (int)(particles[i].x - particles[i].vx * 0.5f) + random(-2, 3);
        int sy = (int)(particles[i].y - particles[i].vy * 0.5f) + random(-2, 3);
        if (sx > 0 && sx < SCREEN_W && sy > 0 && sy < SCREEN_H)
          gfx->drawPixel(sx, sy, RGB565_WHITE);
      }
    }

    gfx->flush();
    if (alive == 0) break;
    delay(20);
  }

  delay(300);
}

// ── screen protocol ─────────────────────────────────────────────────────────

enum ElementType { ELEM_LABEL, ELEM_BUTTON, ELEM_GAP };

struct ScreenElement {
  ElementType type;
  char text[64];
  char id[16];
  uint16_t color;
  uint16_t bg;
  uint16_t border;
  bool hasBorder;
  uint8_t size;
  uint8_t align;  // 0=left, 1=center, 2=right
  int16_t h;
};

struct TouchZone {
  char id[16];
  int16_t x, y, w, h;
};

static ScreenElement screenElements[16];
static int screenElementCount = 0;
static TouchZone touchZones[8];
static int touchZoneCount = 0;

static void screenClear() {
  screenElementCount = 0;
  touchZoneCount = 0;
}

static bool screenAdd(const String &json) {
  if (screenElementCount >= 16) return false;

  JsonDocument doc;
  if (deserializeJson(doc, json)) return false;

  ScreenElement &el = screenElements[screenElementCount];
  memset(&el, 0, sizeof(el));

  const char *type = doc["type"] | "";
  bool colorFound;

  if (strcmp(type, "label") == 0) {
    el.type = ELEM_LABEL;
    strlcpy(el.text, doc["text"] | "", sizeof(el.text));
    el.size = constrain(doc["size"] | 2, 1, 4);
    el.color = colorByName(String(doc["color"] | "white"), &colorFound);
    const char *align = doc["align"] | "left";
    el.align = (strcmp(align, "center") == 0) ? 1
             : (strcmp(align, "right") == 0)  ? 2 : 0;
    el.h = el.size * 8 + 4;
  } else if (strcmp(type, "button") == 0) {
    el.type = ELEM_BUTTON;
    strlcpy(el.text, doc["text"] | "", sizeof(el.text));
    strlcpy(el.id, doc["id"] | "", sizeof(el.id));
    if (el.id[0] == '\0') return false;
    el.size = 2;
    el.color = colorByName(String(doc["color"] | "black"), &colorFound);
    el.bg = colorByName(String(doc["bg"] | "white"), &colorFound);
    const char *bdr = doc["border"];
    if (bdr) {
      el.border = colorByName(String(bdr), &colorFound);
      el.hasBorder = colorFound;
    }
    el.h = constrain(doc["h"] | 50, 20, 200);
  } else if (strcmp(type, "gap") == 0) {
    el.type = ELEM_GAP;
    el.h = constrain(doc["h"] | 10, 1, 200);
  } else {
    return false;
  }

  screenElementCount++;
  return true;
}

void drawMiniLogo(const char *subtitle) {
  gfx->fillScreen(RGB565_BLACK);

  gfx->setTextSize(4);
  gfx->setTextColor(CYAN_TXT);
  gfx->setCursor((320 - 5 * 24) / 2, 5);
  gfx->print("SWAMP");

  gfx->setTextSize(2);
  gfx->setTextColor(PINK_TXT);
  gfx->setCursor((320 - (int)strlen(subtitle) * 12) / 2, 42);
  gfx->print(subtitle);

  gfx->fillCircle(160, 90, 14, BODY_DARK);
  gfx->fillCircle(160, 108, 22, BODY_DARK);
  gfx->fillRect(138, 90, 44, 22, BODY_DARK);

  for (int a = -170; a <= -10; a += 8) {
    float r1 = a * 0.01745f, r2 = (a + 8) * 0.01745f;
    gfx->drawLine(
      160 + (int)(cosf(r1) * 15), 90 + (int)(sinf(r1) * 15),
      160 + (int)(cosf(r2) * 15), 90 + (int)(sinf(r2) * 15),
      BODY_GLOW);
  }

  gfx->fillTriangle(147, 95, 152, 91, 157, 95, EYE_YELLOW);
  gfx->fillTriangle(147, 95, 152, 99, 157, 95, EYE_YELLOW);
  gfx->fillTriangle(163, 95, 168, 91, 173, 95, EYE_YELLOW);
  gfx->fillTriangle(163, 95, 168, 99, 173, 95, EYE_YELLOW);

  int waterY = 125;
  gfx->fillRect(0, waterY, 320, 35, RGB565_BLACK);
  gfx->drawFastHLine(0, waterY, 320, BODY_GLOW);
  gfx->drawFastHLine(0, waterY + 1, 320, CYAN_DIM);
  gfx->drawFastHLine(130, waterY + 4, 60, CYAN_DIM);
  gfx->drawFastHLine(110, waterY + 8, 100, CYAN_DIM);
  gfx->drawFastHLine(85, waterY + 13, 150, CYAN_DIM);
}

static void screenShow() {
  drawMiniLogo("APPROVE");

  const int gutter = 10;
  const int usableW = 320 - 2 * gutter;
  const int gap = 6;
  int y = 170;
  touchZoneCount = 0;

  for (int i = 0; i < screenElementCount; i++) {
    ScreenElement &el = screenElements[i];
    if (y + el.h > 480) break;

    switch (el.type) {
      case ELEM_LABEL: {
        gfx->setTextSize(el.size);
        gfx->setTextColor(el.color);
        int charW = el.size * 6;
        int textW = strlen(el.text) * charW;
        int tx = gutter;
        if (el.align == 1) tx = (320 - textW) / 2;
        else if (el.align == 2) tx = 320 - gutter - textW;
        gfx->setCursor(tx, y);
        gfx->print(el.text);
        y += el.h + gap;
        break;
      }
      case ELEM_BUTTON: {
        if (el.hasBorder) {
          gfx->fillRect(gutter, y, usableW, el.h, RGB565_BLACK);
          gfx->fillRect(gutter, y, usableW, 2, el.border);
          gfx->fillRect(gutter, y + el.h - 2, usableW, 2, el.border);
          gfx->fillRect(gutter, y, 2, el.h, el.border);
          gfx->fillRect(gutter + usableW - 2, y, 2, el.h, el.border);
        } else {
          gfx->fillRect(gutter, y, usableW, el.h, el.bg);
        }
        gfx->setTextSize(2);
        gfx->setTextColor(el.color);
        int textW = strlen(el.text) * 12;
        gfx->setCursor((320 - textW) / 2, y + (el.h - 16) / 2);
        gfx->print(el.text);
        if (touchZoneCount < 8) {
          TouchZone &z = touchZones[touchZoneCount++];
          strlcpy(z.id, el.id, sizeof(z.id));
          z.x = gutter;
          z.y = y;
          z.w = usableW;
          z.h = el.h;
        }
        y += el.h + gap;
        break;
      }
      case ELEM_GAP:
        y += el.h;
        break;
    }
  }

  gfx->flush();
}

// A blocking wait must not deafen the board: any line arriving on serial ends
// it. The line is read and discarded so it cannot be answered twice.
static bool serialInterrupt() {
  if (!Serial.available()) return false;
  unsigned long t0 = millis();
  while (millis() - t0 < 200) {
    if (Serial.available()) {
      if (Serial.read() == '\n') break;
    } else {
      delay(1);
    }
  }
  return true;
}

static void screenWait(unsigned long timeoutMs) {
  unsigned long start = millis();
  bool prevTouch = false;

  while (millis() - start < timeoutMs) {
    setupService();
    if (serialInterrupt()) {
      Serial.println("{\"ok\":true,\"aborted\":true}");
      return;
    }
    int tx, ty, gesture;
    bool touching = readTouch(&tx, &ty, &gesture);
    if (touching && !prevTouch) {
      for (int i = 0; i < touchZoneCount; i++) {
        TouchZone &z = touchZones[i];
        if (tx >= z.x && tx < z.x + z.w && ty >= z.y && ty < z.y + z.h) {
          bool isReject = (strcmp(z.id, "reject") == 0);
          bool isApprove = (strcmp(z.id, "approve") == 0);
          if (isReject || isApprove) {
            // A decision gets a full-screen banner the moment it is tapped.
            uint16_t hiColor = isReject ? PINK_TXT : CYAN_TXT;
            const char *label = isReject ? "REJECTED" : "APPROVED";
            gfx->fillScreen(RGB565_BLACK);
            gfx->fillRect(0, 180, 320, 80, hiColor);
            gfx->setTextSize(3);
            gfx->setTextColor(RGB565_BLACK);
            int tw = strlen(label) * 18;
            gfx->setCursor((320 - tw) / 2, 200);
            gfx->print(label);
          } else {
            // Any other zone just shows it was hit; the host decides what
            // the tap means and draws the next screen.
            for (int t = 0; t < 4; t++) {
              gfx->drawRect(z.x - t, z.y - t, z.w + 2 * t, z.h + 2 * t,
                            RGB565_WHITE);
            }
          }
          gfx->flush();
          Serial.printf("{\"ok\":true,\"id\":\"%s\",\"x\":%d,\"y\":%d}\n",
                        z.id, tx, ty);
          return;
        }
      }
    }
    prevTouch = touching;
    delay(50);
  }

  Serial.println("{\"ok\":true,\"timeout\":true}");
}

// ── touch game ──────────────────────────────────────────────────────────────

static uint32_t gameDisplayedCount = 0;

static void drawGameScreen(uint32_t count) {
  gfx->fillScreen(RGB565_BLACK);

  // title
  gfx->setTextColor(CYAN_TXT);
  gfx->setTextSize(3);
  gfx->setCursor(60, 30);
  gfx->print("TAP GAME");

  // big counter
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(8);
  char num[12];
  snprintf(num, sizeof(num), "%u", (unsigned)count);
  int numW = strlen(num) * 48;
  gfx->setCursor((SCREEN_W - numW) / 2, 180);
  gfx->print(num);

  // instruction
  gfx->setTextColor(PINK_TXT);
  gfx->setTextSize(2);
  gfx->setCursor(55, 420);
  gfx->print("tap the screen!");

  gfx->flush();
  gameDisplayedCount = count;
}

// ── command handler ──────────────────────────────────────────────────────────

static void handle(const String &lineIn) {
  String cmd = lineIn;
  String arg = "";
  int sp = lineIn.indexOf(' ');
  if (sp >= 0) {
    cmd = lineIn.substring(0, sp);
    arg = lineIn.substring(sp + 1);
  }

  if (devState == ST_HOST) lastHostMs = millis();
  // A command that draws or waits hands the screen to the host.
  if (drawsOrWaits(cmd)) {
    enterHost();
    logoMode = false;
    gameMode = false;
  }

  if (cmd == "ping") {
    Serial.printf("{\"ok\":true,\"fw\":\"%s\"}\n", FW);
    return;
  }
  if (cmd == "status") {
    Serial.printf(
        "{\"ok\":true,\"display\":%s,\"w\":%d,\"h\":%d,\"psram\":%u,"
        "\"backlight\":%s,\"touch\":%s,\"touchCount\":%u,\"state\":\"%s\",\"heap\":%u}\n",
        displayOk ? "true" : "false", SCREEN_W, SCREEN_H,
        (unsigned)ESP.getPsramSize(), backlightOn ? "true" : "false",
        touchOk ? "true" : "false", touchCount, stateName(devState),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return;
  }
  if (cmd == "text") {
    int n = drawText(arg);
    Serial.printf("{\"ok\":%s,\"lines\":%d}\n", displayOk ? "true" : "false", n);
    return;
  }
  if (cmd == "clear") {
    if (displayOk) {
      gfx->fillScreen(RGB565_BLACK);
      gfx->flush();
    }
    Serial.printf("{\"ok\":%s}\n", displayOk ? "true" : "false");
    return;
  }
  if (cmd == "fill") {
    bool found = false;
    uint16_t c = colorByName(arg, &found);
    if (!found) {
      Serial.println("{\"ok\":false,\"error\":\"unknown color\"}");
      return;
    }
    if (displayOk) {
      gfx->fillScreen(c);
      gfx->flush();
    }
    Serial.printf("{\"ok\":%s,\"color\":\"%s\"}\n", displayOk ? "true" : "false", arg.c_str());
    return;
  }
  if (cmd == "touch") {
    int tx, ty, gesture;
    if (readTouch(&tx, &ty, &gesture)) {
      latchTouch(tx, ty);
      Serial.printf("{\"ok\":true,\"points\":1,\"x\":%d,\"y\":%d,\"gesture\":%d}\n",
                    tx, ty, gesture);
    } else {
      Serial.println("{\"ok\":true,\"points\":0}");
    }
    return;
  }
  if (cmd == "touchstate") {
    Serial.printf("{\"ok\":true,\"count\":%u,\"x\":%d,\"y\":%d,\"t\":%lu}\n",
                  touchCount, touchLastX, touchLastY, touchLastMs);
    return;
  }
  if (cmd == "touchclear") {
    uint32_t prev = touchCount;
    touchCount = 0;
    Serial.printf("{\"ok\":true,\"cleared\":%u}\n", prev);
    return;
  }
  if (cmd == "waittouch") {
    unsigned long timeout = arg.length() ? arg.toInt() : 30000;
    if (timeout <= 0) timeout = 30000;
    unsigned long start = millis();
    while (millis() - start < timeout) {
      setupService();
      if (serialInterrupt()) {
        Serial.println("{\"ok\":true,\"points\":0,\"aborted\":true}");
        return;
      }
      int tx, ty, gesture;
      if (readTouch(&tx, &ty, &gesture)) {
        latchTouch(tx, ty);
        Serial.printf("{\"ok\":true,\"points\":1,\"x\":%d,\"y\":%d,\"gesture\":%d,\"waitMs\":%lu}\n",
                      tx, ty, gesture, millis() - start);
        return;
      }
      delay(20);
    }
    Serial.println("{\"ok\":true,\"points\":0,\"timeout\":true}");
    return;
  }
  if (cmd == "simtouch") {
    int sx = 0, sy = 0;
    int sep = arg.indexOf(' ');
    if (sep < 0) {
      Serial.println("{\"ok\":false,\"error\":\"simtouch X Y\"}");
      return;
    }
    sx = arg.substring(0, sep).toInt();
    sy = arg.substring(sep + 1).toInt();
    latchTouch(sx, sy);
    Serial.printf("{\"ok\":true,\"simulated\":true,\"x\":%d,\"y\":%d,\"count\":%u}\n",
                  sx, sy, touchCount);
    return;
  }
  if (cmd == "touchgame") {
    if (displayOk && touchOk) {
      touchCount = 0;
      gameMode = true;
      drawGameScreen(0);
      Serial.printf("{\"ok\":true,\"game\":\"started\"}\n");
    } else {
      Serial.printf("{\"ok\":false,\"error\":\"need display and touch\"}\n");
    }
    return;
  }
  if (cmd == "events") {
    eventsOn = (arg == "on");
    Serial.printf("{\"ok\":true,\"events\":%s}\n", eventsOn ? "true" : "false");
    return;
  }
  if (cmd == "logo") {
    if (displayOk) {
      drawLogo();
      logoMode = true;
    }
    Serial.printf("{\"ok\":%s}\n", displayOk ? "true" : "false");
    return;
  }
  if (cmd == "screen") {
    String sub = arg;
    String subArg = "";
    int sp2 = arg.indexOf(' ');
    if (sp2 >= 0) {
      sub = arg.substring(0, sp2);
      subArg = arg.substring(sp2 + 1);
    }

    if (sub == "clear") {
      screenClear();
      Serial.println("{\"ok\":true}");
      return;
    }
    if (sub == "add") {
      if (screenAdd(subArg)) {
        Serial.printf("{\"ok\":true,\"index\":%d}\n", screenElementCount - 1);
      } else {
        Serial.println("{\"ok\":false,\"error\":\"invalid element\"}");
      }
      return;
    }
    if (sub == "show") {
      if (!displayOk) {
        Serial.println("{\"ok\":false,\"error\":\"no display\"}");
        return;
      }
      screenShow();
      Serial.printf("{\"ok\":true,\"elements\":%d,\"zones\":%d}\n",
                    screenElementCount, touchZoneCount);
      return;
    }
    if (sub == "wait") {
      unsigned long ms = subArg.length() > 0 ? subArg.toInt() : 300000;
      if (ms == 0) ms = 300000;
      screenWait(ms);
      return;
    }
    Serial.printf("{\"ok\":false,\"error\":\"unknown screen command: %s\"}\n",
                  sub.c_str());
    return;
  }
  if (cmd == "backlight") {
    if (arg != "on" && arg != "off") {
      Serial.println("{\"ok\":false,\"error\":\"backlight on|off\"}");
      return;
    }
    setBacklight(arg == "on");
    Serial.printf("{\"ok\":true,\"backlight\":%s}\n", backlightOn ? "true" : "false");
    return;
  }
  if (cmd == "wifi") {
    if (arg == "status") {
      netStatusJson(Serial, stateName(devState));
    } else if (arg == "scan") {
      if (WiFi.getMode() == WIFI_OFF) WiFi.mode(WIFI_STA);
      int n = WiFi.scanNetworks(false, true);
      JsonDocument d;
      d["ok"] = true;
      d["count"] = n < 0 ? 0 : n;
      // [ssid, channel, rssi] per network: no nested objects, so a host that
      // ends a reply at a closing brace still reads it whole.
      JsonArray nets = d["networks"].to<JsonArray>();
      for (int i = 0; i < n; i++) {
        JsonArray o = nets.add<JsonArray>();
        o.add(WiFi.SSID(i));
        o.add(WiFi.channel(i));
        o.add(WiFi.RSSI(i));
      }
      WiFi.scanDelete();
      serializeJson(d, Serial);
      Serial.println();
    } else {
      replyError("wifi status|scan");
    }
    return;
  }
  if (cmd == "config") {
    handleConfig(arg);
    return;
  }
  if (cmd == "setup" && arg == "status") {
    portalStatusJson(Serial);
    return;
  }
  if (cmd == "idle") {
    leaveHost();
    Serial.printf("{\"ok\":true,\"state\":\"%s\"}\n", stateName(devState));
    return;
  }
  if (cmd == "profile") {
    if (arg == "status") {
      profileStatusJson(Serial);
    } else if (arg == "refresh") {
      profileRefresh();
      profileStatusJson(Serial);
    } else {
      replyError("profile status|refresh");
    }
    return;
  }
  Serial.printf("{\"ok\":false,\"error\":\"unknown command: %s\"}\n", cmd.c_str());
}

// ── setup & loop ─────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);

  ledcAttach(PIN_BL, BL_FREQ, BL_BITS);
  setBacklight(false);

  displayOk = gfx->begin();
  if (displayOk) {
    drawLogo();
    logoMode = false;
  }
  setBacklight(true);

  // Touch setup AFTER display init.
  pinMode(TOUCH_INT, INPUT_PULLUP);
  pinMode(TOUCH_RST, OUTPUT);
  digitalWrite(TOUCH_RST, LOW);
  delay(200);
  digitalWrite(TOUCH_RST, HIGH);
  delay(200);
  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  Wire.setClock(TOUCH_I2C_CLOCK);

  Wire.beginTransmission(TOUCH_ADDR);
  touchOk = (Wire.endTransmission() == 0);

  netInit();
  configLoad(cfg);
  profileBegin(cfg.api, cfg.profile);
  netBegin(cfg.ssid, cfg.pass);
  if (!configComplete(cfg)) {
    enterSetup();
  } else {
    devState = profileHave() ? ST_BADGE : ST_CONNECTING;
    showState();
  }

  Serial.printf("{\"ok\":%s,\"fw\":\"%s\",\"display\":%s,\"touch\":%s,\"psram\":%u,\"state\":\"%s\"}\n",
                displayOk ? "true" : "false", FW, displayOk ? "true" : "false",
                touchOk ? "true" : "false", (unsigned)ESP.getPsramSize(), stateName(devState));
}

void loop() {
  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '\n') {
      buf.trim();
      if (buf.length()) handle(buf);
      buf = "";
    } else if (ch != '\r') {
      buf += ch;
    }
  }

  unsigned long now = millis();
  netLoop();
  if (devState == ST_HOST && now - lastHostMs > HOST_IDLE_MS) leaveHost();
  if (devState == ST_CONNECTING && netConnected()) {
    devState = ST_BADGE;
    if (displayOk) uiBadgeEnter();
  }
  profileLoop(netConnected(), devState != ST_HOST && devState != ST_SETUP);
  if (inSetup()) {
    setupService();
    PortalPhase ph = portalPhase();
    if (ph == PORTAL_DONE && devState == ST_HOST) {
      leaveSetup();  // finished from the phone while swamp holds the screen
    } else if (devState != ST_SETUP) {
      // swamp holds the screen: the rest waits for the hand-back
    } else if (ph == PORTAL_DONE) {
      if (!setupDoneMs) {
        setupDoneMs = millis();
        if (displayOk) uiHello(profileData());
      } else if (millis() - setupDoneMs > 10000) {  // time for the phone to load "Done"
        leaveSetup();
      }
    } else if (ph == PORTAL_IDLE) {
      bool open = netApClients() > 0;
      if (open != setupShowingOpen) {
        setupShowingOpen = open;
        showState();
      }
    }
  }
  if (devState == ST_BADGE && displayOk) uiBadgeLoop();

  // Touch: latch every press as before; badge taps and the setup hold come
  // from the debounced filter, since the controller drops out mid-press.
  static bool wasTouching = false;
  static TapFilter tapFilter;  // debounced taps and holds (see util.h)
  static int pressX = 0, pressY = 0;
  if (touchOk) {
    int tx, ty, gesture;
    bool touching = readTouch(&tx, &ty, &gesture);
    if (touching && !wasTouching) {
      latchTouch(tx, ty);
      pressX = tx;
      pressY = ty;
      if (eventsOn) {
        Serial.printf("{\"event\":\"tap\",\"n\":%u,\"x\":%d,\"y\":%d,\"t\":%lu}\n",
                      touchCount, tx, ty, touchLastMs);
      }
      if (devState == ST_HOST && logoMode && displayOk) {
        spawnExplosion(tx, ty);
        runExplosion();
        drawLogo();
      }
    }
    TouchEvent ev = tapFilterFeed(tapFilter, touching, millis());
    bool holdable = devState == ST_CONNECTING || (devState == ST_BADGE && uiBadgeOnLogo());
    if (ev == TOUCH_HOLD && holdable) enterSetup();
    if (ev == TOUCH_TAP && displayOk) {
      if (devState == ST_BADGE) {
        profileNoteTap();
        if (!uiBadgeTap()) {
          spawnExplosion(pressX, pressY);
          runExplosion();
          uiBadgeEnter();
        }
      } else if (devState == ST_CONNECTING) {
        spawnExplosion(pressX, pressY);
        runExplosion();
        uiConnecting();
      }
    }
    wasTouching = touching;
  }

  if (gameMode && displayOk && touchCount != gameDisplayedCount) {
    drawGameScreen(touchCount);
  }

  delay(5);
}
