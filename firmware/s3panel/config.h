// The panel's settings and its cached profile, in NVS (survives reflashing
// the app). Nothing here is ever printed: callers report key names only.
#pragma once
#include "profile_data.h"

#define DEFAULT_API "https://swamp-club.com"

struct PanelConfig {
  char ssid[33];
  char pass[65];
  char profile[40];
  char api[96];
};

void configLoad(PanelConfig &c);
bool configComplete(const PanelConfig &c);  // Wi-Fi and a profile are set
void configSaveWifi(const char *ssid, const char *pass);
void configSaveProfile(const char *profile);
void configSaveApi(const char *api);  // "" restores the default
void configForget();                  // settings and cache
bool cacheLoad(Profile &p);           // false: none, or an older layout
void cacheSave(const Profile &p);     // writes only when the data changed
