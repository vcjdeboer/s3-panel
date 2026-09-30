// Pure helpers with no Arduino dependency, so they build and test on the host
// (test/run.sh). Everything that turns API data into screen text lives here.
#pragma once
#include <stddef.h>
#include <stdint.h>

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
// Lower-case letters and digits without 0 1 i l o; out needs len + 1 bytes.
void randomPassword(uint32_t (*rnd)(), char *out, size_t len);
