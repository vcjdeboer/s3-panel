#include <ArduinoJson.h>

#include <fstream>
#include <sstream>
#include <string>

#include "check.h"
#include "profile_parse.h"

static std::string slurp(const char *path) {
  std::ifstream f(path);
  std::stringstream s;
  s << f.rdbuf();
  return s.str();
}

static bool loadUserText(const std::string &text, Profile &p) {
  JsonDocument filter, doc;
  userFilter(filter);
  if (deserializeJson(doc, text, DeserializationOption::Filter(filter))) return false;
  return parseUser(doc, p);
}

static bool loadUser(const char *path, Profile &p) {
  std::string text = slurp(path);
  CHECK(!text.empty());
  return loadUserText(text, p);
}

static bool loadActivity(const char *path, Profile &p) {
  JsonDocument filter, doc;
  activityFilter(filter);
  std::string text = slurp(path);
  CHECK(!text.empty());
  if (deserializeJson(doc, text, DeserializationOption::Filter(filter))) return false;
  return parseActivity(doc, p);
}

int main() {
  static Profile p;

  memset(&p, 0, sizeof p);
  CHECK(loadUser("fixtures/user.json", p));
  CHECK_STR(p.username, "example");
  CHECK(p.points == 1234567);
  CHECK_STR(p.rank, "Bog Keeper");
  CHECK(p.tier == 12);
  CHECK(p.activeDays == 100);
  CHECK(p.totalEvents == 54321);
  CHECK(p.streak == 42);
  CHECK(p.badgeTotal == 3);
  CHECK(p.badgeCount == 3);
  // Newest first, whatever order the API used; the middle dot is ASCII now.
  CHECK_STR(p.badges[0].name, "Marshlight - Prismatic");
  CHECK_STR(p.badges[0].visual, "quest-free");
  CHECK_STR(p.badges[1].name, "Quality Initiate");
  CHECK_STR(p.badges[2].name, "OG Swamper");
  CHECK(p.badges[2].awardedAt == 1780861035);

  // More badges than fit: the newest 20 are kept, the total is still counted.
  std::string many = "{\"username\":\"example\",\"canonicalScore\":1,\"badges\":[";
  for (int i = 0; i < 25; i++) {
    char one[128];
    snprintf(one, sizeof one,
             "%s{\"name\":\"b%d\",\"visual\":\"og\",\"awardedAt\":\"2026-01-%02dT00:00:00Z\"}",
             i ? "," : "", i, i + 1);
    many += one;
  }
  many += "]}";
  memset(&p, 0, sizeof p);
  CHECK(loadUserText(many, p));
  CHECK(p.badgeTotal == 25);
  CHECK(p.badgeCount == PROFILE_MAX_BADGES);
  CHECK_STR(p.badges[0].name, "b24");
  CHECK_STR(p.badges[19].name, "b5");

  // Review Focus 4: a new user is a valid, empty profile, not an error.
  memset(&p, 0, sizeof p);
  CHECK(loadUser("fixtures/user_new.json", p));
  CHECK(p.points == 0);
  CHECK(p.badgeTotal == 0);
  CHECK(p.badgeCount == 0);

  // The API changed shape: refuse rather than show zeros.
  memset(&p, 0, sizeof p);
  CHECK(!loadUser("fixtures/user_changed.json", p));

  memset(&p, 0, sizeof p);
  CHECK(loadActivity("fixtures/activity.json", p));
  CHECK(p.activityCount == 2);
  CHECK_STR(p.activity[0].title, "model method x2");
  CHECK(p.activity[0].amount == 333);
  CHECK(p.activity[0].at == 1790766990);
  CHECK_STR(p.activity[1].title, "workflow validate x8");

  memset(&p, 0, sizeof p);
  CHECK(loadActivity("fixtures/activity_empty.json", p));
  CHECK(p.activityCount == 0);

  CHECK(!loadActivity("fixtures/user_changed.json", p));

  return finish();
}
