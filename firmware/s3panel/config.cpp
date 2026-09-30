#include "config.h"

#include <Preferences.h>
#include <string.h>

static const char *NS_CONFIG = "s3panel";
static const char *NS_CACHE = "badge";

static void readKey(Preferences &p, const char *key, char *out, size_t cap, const char *dflt) {
  String v = p.getString(key, dflt);
  strlcpy(out, v.c_str(), cap);
}

void configLoad(PanelConfig &c) {
  Preferences p;
  p.begin(NS_CONFIG, true);  // read-only; a missing namespace yields the defaults
  readKey(p, "ssid", c.ssid, sizeof c.ssid, "");
  readKey(p, "pass", c.pass, sizeof c.pass, "");
  readKey(p, "profile", c.profile, sizeof c.profile, "");
  readKey(p, "api", c.api, sizeof c.api, DEFAULT_API);
  p.end();
}

bool configComplete(const PanelConfig &c) { return c.ssid[0] && c.profile[0]; }

void configSaveWifi(const char *ssid, const char *pass) {
  Preferences p;
  p.begin(NS_CONFIG, false);
  p.putString("ssid", ssid);
  p.putString("pass", pass);
  p.end();
}

void configSaveProfile(const char *profile) {
  Preferences p;
  p.begin(NS_CONFIG, false);
  p.putString("profile", profile);
  p.end();
}

void configSaveApi(const char *api) {
  Preferences p;
  p.begin(NS_CONFIG, false);
  if (api[0]) p.putString("api", api);
  else p.remove("api");
  p.end();
}

void configForget() {
  Preferences p;
  p.begin(NS_CONFIG, false);
  p.clear();
  p.end();
  p.begin(NS_CACHE, false);
  p.clear();
  p.end();
}

bool cacheLoad(Profile &out) {
  Preferences p;
  p.begin(NS_CACHE, true);
  bool ok = p.getBytesLength("profile") == sizeof(Profile) &&
            p.getBytes("profile", &out, sizeof(Profile)) == sizeof(Profile) &&
            out.layout == PROFILE_LAYOUT;
  p.end();
  return ok;
}

void cacheSave(const Profile &fresh) {
  static Profile old;  // static: 2 KB off the loop task's stack
  if (cacheLoad(old)) {
    old.fetchedAt = fresh.fetchedAt;  // a new timestamp alone is not a change
    if (memcmp(&old, &fresh, sizeof(Profile)) == 0) return;
  }
  Preferences p;
  p.begin(NS_CACHE, false);
  p.putBytes("profile", &fresh, sizeof(Profile));
  p.end();
}
