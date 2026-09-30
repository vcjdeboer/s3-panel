#include "badge_ui.h"

#include <string.h>

#include "net.h"
#include "panel.h"
#include "profile.h"
#include "qrcode.h"
#include "util.h"

enum UiView { VIEW_LOGO, VIEW_PROFILE, VIEW_BADGES, VIEW_ACTIVITY };

static UiView view = VIEW_LOGO;
static unsigned long lastTapMs = 0;
static uint8_t level = 100;
static uint32_t drawnGeneration = 0;

static const unsigned long DIM_MS = 5UL * 60 * 1000;   // any page: 15 %
static const unsigned long OFF_MS = 10UL * 60 * 1000;  // any page: dark
static const unsigned long HIGHLIGHT_MS = 2UL * 1000;  // new activity rows

// Activity rows that arrived recently, highlighted until `until` (by content,
// so they stay marked however the list shifts). Tracked on every page.
struct Fresh {
  Activity a;
  unsigned long until;
};
static Fresh fresh[PROFILE_MAX_ACTIVITY];
static Activity knownActivity[PROFILE_MAX_ACTIVITY];
static int knownCount = -1;  // -1: nothing seen yet (first load highlights nothing)
static bool drawnWithFresh = false;
static unsigned long drawnAtMs = 0;
static const unsigned long AGES_MS = 10UL * 1000;  // activity page: refresh the "4m ago" ages

static void textAt(int x, int y, uint8_t size, uint16_t color, const char *s) {
  gfx->setTextSize(size);
  gfx->setTextColor(color);
  gfx->setCursor(x, y);
  gfx->print(s);
}

static void centered(int y, uint8_t size, uint16_t color, const char *s) {
  char buf[64];
  fitText(s, 312 / (6 * size), buf, sizeof buf);
  int w = (int)strlen(buf) * 6 * size;
  textAt((SCREEN_W - w) / 2, y, size, color, buf);
}

static void setLevel(uint8_t pct) {
  level = pct;
  setBacklightPct(pct);
}

// Why the data may be wrong or old: "" when it is fresh. Drawn large on the
// profile card, because it changes what the numbers mean.
static void problemNote(char *out, size_t cap) {
  char age[16];
  relativeAge(netNow(), profileData().fetchedAt, age, sizeof age);
  FetchResult r = profileLast();
  out[0] = '\0';
  if (r == FETCH_NOT_FOUND) snprintf(out, cap, "user not found");
  else if (r == FETCH_PARSE) snprintf(out, cap, "data error");
  else if (r == FETCH_CONNECT || r == FETCH_HTTP || !netConnected())
    snprintf(out, cap, age[0] ? "offline - %s" : "offline", age);
}

// The small footer: what a tap does, and how fresh the data is.
static void footerNote(const char *lead, bool problem, char *out, size_t cap) {
  char age[16];
  relativeAge(netNow(), profileData().fetchedAt, age, sizeof age);
  if (!problem && age[0]) snprintf(out, cap, "%s - updated %s", lead, age);
  else snprintf(out, cap, "%s", lead);
}

// The stored user no longer exists on swamp-club: its old numbers would only
// mislead, so show the name, the problem and the way out instead.
static void drawNotFound() {
  drawMiniLogo("PROFILE");
  centered(175, 3, RGB565_WHITE, profileUsername());  // what was looked up
  gfx->fillRect(0, 225, SCREEN_W, 40, PINK_TXT);
  centered(237, 2, RGB565_BLACK, "user not found");
  centered(300, 2, RGB565_WHITE, "check the name on");
  centered(325, 2, RGB565_WHITE, "swamp-club");
  centered(380, 2, CYAN_DIM, "hold 5 s on the logo");
  centered(405, 2, CYAN_DIM, "to set up again");
  centered(462, 1, CYAN_DIM, "tap for logo");
  gfx->flush();
}

static void drawProfile() {
  if (profileLast() == FETCH_NOT_FOUND) {
    drawNotFound();
    return;
  }
  const Profile &p = profileData();
  char buf[64], num[24];
  drawMiniLogo("PROFILE");
  centered(165, 3, RGB565_WHITE, p.username);
  formatThousands(p.points, num, sizeof num);
  centered(205, 4, CYAN_TXT, num);
  centered(245, 2, CYAN_DIM, "points");
  if (p.rank[0]) snprintf(buf, sizeof buf, "%s - tier %d", p.rank, (int)p.tier);
  else snprintf(buf, sizeof buf, "tier %d", (int)p.tier);
  centered(285, 2, PINK_TXT, buf);
  snprintf(buf, sizeof buf, "%d days  %d streak", (int)p.activeDays, (int)p.streak);
  centered(325, 2, RGB565_WHITE, buf);
  formatThousands(p.totalEvents, num, sizeof num);
  snprintf(buf, sizeof buf, "%s events", num);
  centered(355, 2, RGB565_WHITE, buf);
  char problem[40];
  problemNote(problem, sizeof problem);
  if (problem[0]) {  // a banner, as for APPROVED/REJECTED: it changes what the numbers mean
    gfx->fillRect(0, 392, SCREEN_W, 40, PINK_TXT);
    centered(404, 2, RGB565_BLACK, problem);
  }
  footerNote("tap for badges", problem[0] != 0, buf, sizeof buf);
  centered(462, 1, CYAN_DIM, buf);
  gfx->flush();
}

static void badgeIcon(const char *visual, int cx, int cy) {
  if (!strcmp(visual, "og")) {
    gfx->fillTriangle(cx - 10, cy, cx, cy - 12, cx + 10, cy, CYAN_TXT);
    gfx->fillTriangle(cx - 10, cy, cx, cy + 12, cx + 10, cy, CYAN_TXT);
  } else if (!strcmp(visual, "quality")) {
    for (int i = 0; i < 6; i++) {
      float a1 = (60 * i - 30) * 0.01745f, a2 = (60 * (i + 1) - 30) * 0.01745f;
      gfx->fillTriangle(cx, cy, cx + (int)(cosf(a1) * 11), cy + (int)(sinf(a1) * 11),
                        cx + (int)(cosf(a2) * 11), cy + (int)(sinf(a2) * 11), PINK_TXT);
    }
  } else if (!strcmp(visual, "i-was-here")) {
    for (int i = 0; i < 5; i++) {
      float a = (72 * i - 90) * 0.01745f, l = a - 0.6f, r = a + 0.6f;
      gfx->fillTriangle(cx + (int)(cosf(a) * 12), cy + (int)(sinf(a) * 12),
                        cx + (int)(cosf(l) * 5), cy + (int)(sinf(l) * 5),
                        cx + (int)(cosf(r) * 5), cy + (int)(sinf(r) * 5), EYE_YELLOW);
    }
    gfx->fillCircle(cx, cy, 5, EYE_YELLOW);
  } else if (!strcmp(visual, "quest-free")) {
    gfx->fillCircle(cx, cy, 10, BODY_GLOW);
  } else if (!strcmp(visual, "quest-capstone")) {
    gfx->fillRect(cx - 10, cy + 2, 21, 7, EYE_YELLOW);
    gfx->fillTriangle(cx - 10, cy + 2, cx - 10, cy - 10, cx - 3, cy + 2, EYE_YELLOW);
    gfx->fillTriangle(cx - 5, cy + 2, cx, cy - 12, cx + 5, cy + 2, EYE_YELLOW);
    gfx->fillTriangle(cx + 3, cy + 2, cx + 10, cy - 10, cx + 10, cy + 2, EYE_YELLOW);
  } else {
    gfx->drawCircle(cx, cy, 10, RGB565_WHITE);
    gfx->drawCircle(cx, cy, 9, RGB565_WHITE);
  }
}

static void drawBadges() {
  const Profile &p = profileData();
  char buf[40], l1[16], l2[16];
  gfx->fillScreen(RGB565_BLACK);
  snprintf(buf, sizeof buf, "BADGES  %u", (unsigned)p.badgeTotal);
  textAt(10, 12, 3, CYAN_TXT, buf);
  if (p.badgeCount == 0) centered(220, 2, RGB565_WHITE, "no badges yet");
  for (int i = 0; i < p.badgeCount; i++) {
    int x = 6 + (i / 10) * 157, y = 52 + (i % 10) * 40;
    badgeIcon(p.badges[i].visual, x + 12, y + 16);
    wrap2(p.badges[i].name, 10, l1, l2, sizeof l1);
    textAt(x + 28, y, 2, RGB565_WHITE, l1);
    textAt(x + 28, y + 17, 2, CYAN_DIM, l2);
  }
  if (p.badgeTotal > p.badgeCount)
    snprintf(buf, sizeof buf, "+%u more - tap", (unsigned)(p.badgeTotal - p.badgeCount));
  else snprintf(buf, sizeof buf, "tap for activity");
  centered(462, 1, CYAN_DIM, buf);
  gfx->flush();
}

static bool isFresh(const Activity &a) {
  unsigned long t = millis();
  for (int i = 0; i < PROFILE_MAX_ACTIVITY; i++) {
    const Fresh &f = fresh[i];
    if (f.until && (long)(f.until - t) > 0 && f.a.at == a.at && f.a.amount == a.amount &&
        strcmp(f.a.title, a.title) == 0)
      return true;
  }
  return false;
}

static bool anyFresh() {
  unsigned long t = millis();
  for (int i = 0; i < PROFILE_MAX_ACTIVITY; i++)
    if (fresh[i].until && (long)(fresh[i].until - t) > 0) return true;
  return false;
}

// Compare the profile's activity with what was seen before; mark new rows.
static void trackActivity() {
  const Profile &p = profileData();
  bool isNew[PROFILE_MAX_ACTIVITY];
  newActivityMask(knownActivity, knownCount < 0 ? 0 : knownCount, p.activity, p.activityCount,
                  isNew);
  for (int i = 0; i < p.activityCount; i++) {
    if (!isNew[i]) continue;
    int slot = 0;  // reuse an expired slot, else the one expiring first
    for (int j = 1; j < PROFILE_MAX_ACTIVITY; j++)
      if (fresh[j].until < fresh[slot].until) slot = j;
    fresh[slot].a = p.activity[i];
    fresh[slot].until = millis() + HIGHLIGHT_MS;
  }
  memcpy(knownActivity, p.activity, sizeof knownActivity);
  knownCount = p.activityCount;
}

static void drawActivity() {
  const Profile &p = profileData();
  char pts[24], line[48], age[16];
  gfx->fillScreen(RGB565_BLACK);
  textAt(10, 12, 3, CYAN_TXT, "ACTIVITY");
  if (p.activityCount == 0) centered(220, 2, RGB565_WHITE, "no activity yet");
  int64_t now = netNow();
  for (int r = 0; r < p.activityCount; r++) {
    // Oldest at the top, newest at the bottom (the API lists newest first).
    const Activity &a = p.activity[p.activityCount - 1 - r];
    int y = 56 + r * 50;
    bool hot = isFresh(a);
    if (hot) gfx->fillRect(0, y - 4, SCREEN_W, 46, CYAN_TXT);
    uint16_t pointsColor = hot ? RGB565_BLACK : CYAN_TXT;
    uint16_t ageColor = hot ? RGB565_BLACK : CYAN_DIM;
    uint16_t titleColor = hot ? RGB565_BLACK : RGB565_WHITE;
    formatThousands(a.amount, pts, sizeof pts);
    snprintf(line, sizeof line, "+%s", pts);
    textAt(10, y, 2, pointsColor, line);
    relativeAge(now, a.at, age, sizeof age);
    textAt(SCREEN_W - 10 - (int)strlen(age) * 6, y + 4, 1, ageColor, age);
    fitText(a.title, 25, line, sizeof line);
    textAt(10, y + 20, 2, titleColor, line);
  }
  drawnWithFresh = anyFresh();
  centered(462, 1, CYAN_DIM, "tap for logo");
  gfx->flush();
}

static void show(UiView v) {
  view = v;
  if (v == VIEW_LOGO) drawLogo();
  else if (v == VIEW_PROFILE) drawProfile();
  else if (v == VIEW_BADGES) drawBadges();
  else drawActivity();
  drawnGeneration = profileGeneration();
  drawnAtMs = millis();
}

void uiBadgeEnter() {
  lastTapMs = millis();
  setLevel(100);
  show(VIEW_LOGO);
}

bool uiBadgeOnLogo() { return view == VIEW_LOGO; }

bool uiBadgeTap() {
  lastTapMs = millis();
  if (level < 100) {  // the first tap on a dim screen only wakes it
    setLevel(100);
    return true;
  }
  if (!profileHave()) return false;
  UiView next = (UiView)((view + 1) % 4);
  if (profileLast() == FETCH_NOT_FOUND && next != VIEW_PROFILE) next = VIEW_LOGO;  // no stale badges
  show(next);
  return true;
}

void uiBadgeLoop() {
  // The page stays where the user left it; only the brightness follows idleness.
  unsigned long idle = millis() - lastTapMs;
  if (idle > OFF_MS) {
    if (level != 0) setLevel(0);
  } else if (idle > DIM_MS) {
    if (level != 15) setLevel(15);
  }
  bool changed = drawnGeneration != profileGeneration();
  if (changed && profileHave()) trackActivity();
  if (view == VIEW_LOGO) {
    drawnGeneration = profileGeneration();
    return;
  }
  // Redraw on new data; on the activity page also when a highlight runs out,
  // and every AGES_MS so the ages keep up.
  if (changed || (view == VIEW_ACTIVITY && ((drawnWithFresh && !anyFresh()) ||
                                           millis() - drawnAtMs >= AGES_MS)))
    show(view);
}

void uiConnecting() {
  setLevel(100);
  view = VIEW_LOGO;
  drawLogo();
  gfx->fillRect(0, 430, SCREEN_W, 28, RGB565_BLACK);
  centered(436, 2, CYAN_TXT, "connecting...");
  gfx->flush();
}

void uiHello(const Profile &p) {
  char num[24], buf[64];
  setLevel(100);
  drawMiniLogo("HELLO");
  snprintf(buf, sizeof buf, "hi %s", p.username);
  centered(200, 3, RGB565_WHITE, buf);
  formatThousands(p.points, num, sizeof num);
  centered(250, 4, CYAN_TXT, num);
  centered(290, 2, CYAN_DIM, "points");
  gfx->flush();
}

static int qrX, qrY, qrBox;

static void qrDraw(esp_qrcode_handle_t q) {
  int n = esp_qrcode_get_size(q);
  int scale = qrBox / (n + 8);  // 4 modules of quiet zone each side
  if (scale < 1) scale = 1;
  int side = scale * (n + 8);
  int ox = qrX + (qrBox - side) / 2, oy = qrY + (qrBox - side) / 2;
  gfx->fillRect(ox, oy, side, side, RGB565_WHITE);
  for (int y = 0; y < n; y++)
    for (int x = 0; x < n; x++)
      if (esp_qrcode_get_module(q, x, y))
        gfx->fillRect(ox + (x + 4) * scale, oy + (y + 4) * scale, scale, scale, RGB565_BLACK);
}

static void drawQr(const char *text, int x, int y, int box) {
  qrX = x;
  qrY = y;
  qrBox = box;
  esp_qrcode_config_t cfg = {};
  cfg.display_func = qrDraw;
  cfg.max_qrcode_version = 10;
  cfg.qrcode_ecc_level = ESP_QRCODE_ECC_MED;
  esp_qrcode_generate(&cfg, text);
}

static void setupHeader(const char *step) {
  setLevel(100);
  view = VIEW_LOGO;
  gfx->fillScreen(RGB565_BLACK);
  textAt(10, 12, 3, CYAN_TXT, "SET UP");
  textAt(10, 52, 2, PINK_TXT, step);
}

void uiSetupJoin(const char *apSsid, const char *apPass) {
  char qr[128], buf[48];
  setupHeader("1 - scan to join");
  wifiQrText(apSsid, apPass, qr, sizeof qr);
  drawQr(qr, 30, 80, 260);
  snprintf(buf, sizeof buf, "network  %s", apSsid);
  textAt(10, 360, 2, RGB565_WHITE, buf);
  snprintf(buf, sizeof buf, "password %s", apPass);
  textAt(10, 390, 2, RGB565_WHITE, buf);
  centered(440, 1, CYAN_DIM, "or join that network by hand");
  gfx->flush();
}

void uiSetupOpen() {
  setupHeader("2 - scan to set up");
  drawQr("http://192.168.4.1/", 30, 80, 260);
  centered(370, 2, RGB565_WHITE, "or open 192.168.4.1");
  centered(440, 1, CYAN_DIM, "or tap this network in Wi-Fi settings");
  gfx->flush();
}

void uiSetupChecking(const char *ssid) {
  setupHeader("checking...");
  centered(200, 2, RGB565_WHITE, "joining");
  centered(230, 2, CYAN_TXT, ssid);
  centered(270, 2, RGB565_WHITE, "and finding your");
  centered(300, 2, RGB565_WHITE, "swamp profile");
  gfx->flush();
}

void uiSetupError(const char *line1, const char *line2) {
  setupHeader("not yet");
  centered(200, 2, PINK_TXT, line1);
  centered(230, 2, RGB565_WHITE, line2);
  centered(300, 2, CYAN_DIM, "fix it on your phone");
  gfx->flush();
}
