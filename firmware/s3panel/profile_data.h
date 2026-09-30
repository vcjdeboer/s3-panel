// A swamp-club profile as the panel shows it. Plain data with fixed-size
// fields, so it can be cached as one NVS blob and compared byte for byte
// (always memset a Profile to 0 before filling it). Text is display ASCII.
#pragma once
#include <stdint.h>

#define PROFILE_LAYOUT 1  // bump when this struct changes: old caches are ignored
#define PROFILE_MAX_BADGES 20
#define PROFILE_MAX_ACTIVITY 8

struct Badge {
  char name[40];
  char visual[16];  // og, quality, i-was-here, quest-free, quest-capstone, ...
  int64_t awardedAt;
};

struct Activity {
  char title[48];
  int32_t amount;
  int64_t at;
};

struct Profile {
  uint32_t layout;
  char username[40];
  int64_t points;
  char rank[24];
  int32_t tier;
  int32_t activeDays;
  int64_t totalEvents;
  int32_t streak;
  uint16_t badgeTotal;  // all badges the user has
  uint8_t badgeCount;   // how many are in badges[], newest first
  Badge badges[PROFILE_MAX_BADGES];
  uint8_t activityCount;
  Activity activity[PROFILE_MAX_ACTIVITY];
  int64_t fetchedAt;  // epoch seconds; 0 when the clock was not set
};
