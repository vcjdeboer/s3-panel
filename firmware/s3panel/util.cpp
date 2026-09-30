#include "util.h"

#include <stdio.h>
#include <string.h>

// Howard Hinnant's days_from_civil: days since 1970-01-01.
static int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned mp = m > 2 ? m - 3 : m + 9;
  const unsigned doy = (153 * mp + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

int64_t parseIso8601(const char *s) {
  if (!s) return -1;
  int y, mo, d, h, mi, se, n = 0;
  if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d%n", &y, &mo, &d, &h, &mi, &se, &n) != 6) return -1;
  if (mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59 ||
      se < 0 || se > 60)
    return -1;
  const char *rest = s + n;
  if (*rest == '.') {
    rest++;
    while (*rest >= '0' && *rest <= '9') rest++;
  }
  if (*rest != 'Z') return -1;
  return daysFromCivil(y, (unsigned)mo, (unsigned)d) * 86400 + h * 3600 + mi * 60 + se;
}

void formatThousands(int64_t v, char *out, size_t cap) {
  char digits[24];
  uint64_t u = v < 0 ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
  int n = snprintf(digits, sizeof digits, "%llu", (unsigned long long)u);
  char tmp[32];
  size_t k = 0;
  if (v < 0) tmp[k++] = '-';
  for (int i = 0; i < n; i++) {
    if (i > 0 && (n - i) % 3 == 0) tmp[k++] = ',';
    tmp[k++] = digits[i];
  }
  tmp[k] = '\0';
  snprintf(out, cap, "%s", tmp);
}

void relativeAge(int64_t now, int64_t then, char *out, size_t cap) {
  if (cap == 0) return;
  out[0] = '\0';
  if (now < CLOCK_VALID_AFTER || then < CLOCK_VALID_AFTER) return;
  int64_t d = now - then;
  if (d < 60) snprintf(out, cap, "just now");
  else if (d < 3600) snprintf(out, cap, "%dm ago", (int)(d / 60));
  else if (d < 86400) snprintf(out, cap, "%dh ago", (int)(d / 3600));
  else snprintf(out, cap, "%dd ago", (int)(d / 86400));
}

static const char *asciiFor(uint32_t cp) {
  switch (cp) {
    case 0x00D7: return "x";
    case 0x00B7: case 0x2022: case 0x2013: case 0x2014: return "-";
    case 0x2018: case 0x2019: return "'";
    case 0x201C: case 0x201D: return "\"";
    case 0x2026: return "...";
    default: return "?";
  }
}

void toDisplayAscii(const char *in, char *out, size_t cap) {
  if (cap == 0) return;
  size_t k = 0;
  const unsigned char *p = (const unsigned char *)(in ? in : "");
  while (*p) {
    uint32_t cp;
    int len;
    if (*p < 0x80) { cp = *p; len = 1; }
    else if ((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; len = 2; }
    else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; len = 3; }
    else if ((*p & 0xF8) == 0xF0) { cp = *p & 0x07; len = 4; }
    else { cp = 0xFFFD; len = 1; }
    for (int i = 1; i < len; i++) {
      if ((p[i] & 0xC0) != 0x80) { cp = 0xFFFD; len = 1; break; }
      cp = (cp << 6) | (p[i] & 0x3F);
    }
    p += len;
    char one[2] = {0, 0};
    const char *rep = one;
    if (cp < 0x80) one[0] = cp < 0x20 ? ' ' : (char)cp;
    else rep = asciiFor(cp);
    size_t rl = strlen(rep);
    if (k + rl >= cap) break;
    memcpy(out + k, rep, rl);
    k += rl;
  }
  out[k] = '\0';
}

void fitText(const char *in, size_t maxChars, char *out, size_t cap) {
  if (cap == 0) return;
  size_t len = strlen(in);
  if (maxChars >= cap) maxChars = cap - 1;
  if (len <= maxChars) {
    memcpy(out, in, len + 1);
    return;
  }
  if (maxChars < 4) {
    memcpy(out, in, maxChars);
    out[maxChars] = '\0';
    return;
  }
  memcpy(out, in, maxChars - 3);
  memcpy(out + maxChars - 3, "...", 4);
}

void wrap2(const char *in, size_t width, char *l1, char *l2, size_t cap) {
  if (width >= cap) width = cap - 1;
  size_t len = strlen(in);
  if (len <= width) {
    memcpy(l1, in, len + 1);
    l2[0] = '\0';
    return;
  }
  size_t cut = width;
  while (cut > 0 && in[cut] != ' ') cut--;
  if (cut == 0) cut = width;  // one long word: hard break
  memcpy(l1, in, cut);
  l1[cut] = '\0';
  const char *rest = in + cut;
  while (*rest == ' ') rest++;
  fitText(rest, width, l2, cap);
}

bool validUsername(const char *s) {
  size_t n = 0;
  for (; s[n]; n++) {
    char c = s[n];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-';
    if (!ok || n >= 39) return false;
  }
  return n >= 1;
}

static size_t putLiteral(char *out, size_t k, size_t cap, const char *lit) {
  size_t n = strlen(lit);
  if (k + n < cap) {
    memcpy(out + k, lit, n);
    k += n;
  }
  return k;
}

static size_t putEscaped(char *out, size_t k, size_t cap, const char *s) {
  for (; *s && k + 2 < cap; s++) {
    if (strchr("\\;,:\"", *s)) out[k++] = '\\';
    out[k++] = *s;
  }
  return k;
}

void wifiQrText(const char *ssid, const char *pass, char *out, size_t cap) {
  if (cap == 0) return;
  size_t k = 0;
  k = putLiteral(out, k, cap, "WIFI:T:WPA;S:");
  k = putEscaped(out, k, cap, ssid);
  k = putLiteral(out, k, cap, ";P:");
  k = putEscaped(out, k, cap, pass);
  k = putLiteral(out, k, cap, ";;");
  out[k] = '\0';
}

void htmlEscape(const char *in, char *out, size_t cap) {
  if (cap == 0) return;
  size_t k = 0;
  for (; *in; in++) {
    const char *rep = nullptr;
    switch (*in) {
      case '&': rep = "&amp;"; break;
      case '<': rep = "&lt;"; break;
      case '>': rep = "&gt;"; break;
      case '"': rep = "&quot;"; break;
      case '\'': rep = "&#39;"; break;
    }
    size_t n = rep ? strlen(rep) : 1;
    if (k + n >= cap) break;
    if (rep) memcpy(out + k, rep, n);
    else out[k] = *in;
    k += n;
  }
  out[k] = '\0';
}

TouchEvent tapFilterFeed(TapFilter &f, bool touching, uint32_t now) {
  if (touching) {
    if (!f.down) {
      f.down = true;
      f.held = false;
      f.pressMs = now;
    }
    f.lastSeenMs = now;
    if (!f.held && now - f.pressMs >= TAP_HOLD_MS) {
      f.held = true;
      return TOUCH_HOLD;
    }
    return TOUCH_NONE;
  }
  if (f.down && now - f.lastSeenMs >= TAP_RELEASE_MS) {
    f.down = false;
    if (!f.held && f.lastSeenMs - f.pressMs < TAP_MAX_MS) return TOUCH_TAP;
  }
  return TOUCH_NONE;
}

const char *joinReasonText(int reason) {
  static char other[16];
  switch (reason) {
    case 0: return "timeout";
    case 201: return "network not found";              // NO_AP_FOUND
    case 2: case 15: case 202: case 204:               // AUTH_EXPIRE, 4WAY_HANDSHAKE_TIMEOUT,
      return "wrong password";                         // AUTH_FAIL, HANDSHAKE_TIMEOUT
    case 203: case 205: return "refused by the router";  // ASSOC_FAIL, CONNECTION_FAIL
    default:
      snprintf(other, sizeof other, "reason %d", reason);
      return other;
  }
}

void randomPassword(uint32_t (*rnd)(), char *out, size_t len) {
  static const char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
  for (size_t i = 0; i < len; i++) out[i] = alphabet[rnd() % (sizeof alphabet - 1)];
  out[len] = '\0';
}
