// Turn swamp-club's public JSON into a Profile. The filters keep only the
// fields below, so the ~50 KB user document is never held whole.
#pragma once
#include <ArduinoJson.h>

#include "profile_data.h"

void userFilter(JsonDocument &f);
void activityFilter(JsonDocument &f);
// false: the document does not look like a swamp-club user (API changed?)
bool parseUser(JsonDocument &doc, Profile &p);
// false: the document has no combat-log entries array
bool parseActivity(JsonDocument &doc, Profile &p);
