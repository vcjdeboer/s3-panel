#include "profile_parse.h"

#include <string.h>

#include "util.h"

static void copyText(const char *src, char *dst, size_t cap) {
  toDisplayAscii(src ? src : "", dst, cap);
}

void userFilter(JsonDocument &f) {
  f["username"] = true;
  f["canonicalScore"] = true;
  f["signInStreak"] = true;
  f["tierData"]["tierName"] = true;
  f["tierData"]["ordinal"] = true;
  f["stats"]["activeDays"] = true;
  f["stats"]["totalEvents"] = true;
  f["badges"][0]["name"] = true;
  f["badges"][0]["visual"] = true;
  f["badges"][0]["awardedAt"] = true;
}

void activityFilter(JsonDocument &f) {
  f["entries"][0]["title"] = true;
  f["entries"][0]["amount"] = true;
  f["entries"][0]["grantedAt"] = true;
}

bool parseUser(JsonDocument &doc, Profile &p) {
  const char *user = doc["username"].as<const char *>();
  JsonVariant score = doc["canonicalScore"];
  if (!user || score.isNull()) return false;
  p.layout = PROFILE_LAYOUT;
  copyText(user, p.username, sizeof p.username);
  p.points = score.as<int64_t>();
  copyText(doc["tierData"]["tierName"].as<const char *>(), p.rank, sizeof p.rank);
  p.tier = doc["tierData"]["ordinal"] | 0;
  p.activeDays = doc["stats"]["activeDays"] | 0;
  p.totalEvents = doc["stats"]["totalEvents"] | (int64_t)0;
  p.streak = doc["signInStreak"] | 0;

  JsonArray badges = doc["badges"];
  p.badgeTotal = badges.size();
  p.badgeCount = 0;
  const size_t cap = 128;
  size_t n = badges.size() < cap ? badges.size() : cap;
  int64_t times[cap];
  bool taken[cap];
  for (size_t i = 0; i < n; i++) {
    times[i] = parseIso8601(badges[i]["awardedAt"].as<const char *>());
    taken[i] = false;
  }
  while (p.badgeCount < PROFILE_MAX_BADGES) {
    int best = -1;
    for (size_t i = 0; i < n; i++) {
      if (!taken[i] && (best < 0 || times[i] > times[best])) best = (int)i;
    }
    if (best < 0) break;
    taken[best] = true;
    Badge &b = p.badges[p.badgeCount++];
    copyText(badges[best]["name"].as<const char *>(), b.name, sizeof b.name);
    copyText(badges[best]["visual"].as<const char *>(), b.visual, sizeof b.visual);
    b.awardedAt = times[best];
  }
  return true;
}

bool parseActivity(JsonDocument &doc, Profile &p) {
  JsonArray entries = doc["entries"];
  if (entries.isNull()) return false;
  p.activityCount = 0;
  for (JsonObject e : entries) {
    if (p.activityCount >= PROFILE_MAX_ACTIVITY) break;
    Activity &a = p.activity[p.activityCount++];
    copyText(e["title"].as<const char *>(), a.title, sizeof a.title);
    a.amount = e["amount"] | 0;
    a.at = parseIso8601(e["grantedAt"].as<const char *>());
  }
  return true;
}
