// Pure helpers with no Arduino dependency, so they build and test on the host
// (test/run.sh). Everything that turns API data into screen text lives here.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "profile_data.h"

// Epoch seconds before this mean SNTP has not set the clock yet.
const int64_t CLOCK_VALID_AFTER = 1700000000;

// "2026-09-30T11:16:30.908Z" -> epoch seconds; -1 if malformed or not UTC.
int64_t parseIso8601(const char *s);
// 5520333 -> "5,520,333"
void formatThousands(int64_t v, char *out, size_t cap);
// "just now", "4m ago", "2h ago", "3d ago"; "" when either time is unset.
void relativeAge(int64_t now, int64_t then, char *out, size_t cap);
// UTF-8 -> the panel font's ASCII: x for U+00D7, - for U+00B7, ? otherwise.
void toDisplayAscii(const char *utf8, char *out, size_t cap);
// At most maxChars characters, ending in "..." when cut.
void fitText(const char *in, size_t maxChars, char *out, size_t cap);
// Two lines of at most `width` characters, broken at a space when possible.
void wrap2(const char *in, size_t width, char *l1, char *l2, size_t cap);
// swamp-club usernames: [A-Za-z0-9._-]{1,39}
bool validUsername(const char *s);
// WIFI:T:WPA;S:<ssid>;P:<pass>;; with \ ; , : " escaped.
void wifiQrText(const char *ssid, const char *pass, char *out, size_t cap);
void htmlEscape(const char *in, char *out, size_t cap);
// The setup form's network: a typed name ("other") wins over the drop-down.
const char *pickSsid(const char *selected, const char *other);
// Touch debouncing. The controller reports "no finger" for a few ms mid-press,
// so a release counts only after TAP_RELEASE_MS without contact. A press of
// under TAP_MAX_MS is a TAP (reported at release); TAP_HOLD_MS of contact is
// one HOLD (reported while still held), and its release is no tap.
enum TouchEvent { TOUCH_NONE, TOUCH_TAP, TOUCH_HOLD };
const uint32_t TAP_RELEASE_MS = 120;
const uint32_t TAP_MAX_MS = 1000;
const uint32_t TAP_HOLD_MS = 5000;
struct TapFilter {
  bool down = false;
  bool held = false;
  uint32_t pressMs = 0;
  uint32_t lastSeenMs = 0;
};
TouchEvent tapFilterFeed(TapFilter &f, bool touching, uint32_t nowMs);

// isNew[i]: now[i] (same time, title and points) is not in before. An empty
// `before` (first load) marks nothing new.
void newActivityMask(const Activity *before, int nb, const Activity *now, int nn, bool *isNew);

// Why a Wi-Fi join failed, from ESP-IDF's disconnect reason (0: no reason
// seen). The returned text is valid until the next call.
const char *joinReasonText(int reason);
// Lower-case letters and digits without 0 1 i l o; out needs len + 1 bytes.
void randomPassword(uint32_t (*rnd)(), char *out, size_t len);
