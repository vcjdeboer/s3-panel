// Fetching a swamp-club profile over HTTPS, on a schedule, with a cache.
#pragma once
#include <Print.h>

#include "profile_data.h"

enum FetchResult { FETCH_NONE, FETCH_OK, FETCH_NOT_FOUND, FETCH_CONNECT, FETCH_HTTP, FETCH_PARSE };

// Both endpoints; `out` is replaced only when both parse.
FetchResult profileFetch(const char *api, const char *username, Profile &out, int &httpCode);
void profileBegin(const char *api, const char *username);  // loads the cache, fetch due now
void profileAdopt(const Profile &p);                       // a fetch made elsewhere (setup)
void profileLoop(bool wifiUp, bool mayFetch);              // starts a due fetch in the background, adopts a finished one
FetchResult profileRefresh();                              // fetch now (blocking)
bool profileWaitIdle(uint32_t timeoutMs);  // before any synchronous profileFetch
void profileNoteTap();                                     // fetch on the next loop if stale
bool profileHave();
const Profile &profileData();
const char *profileUsername();  // the stored name being looked up (not fetched data)
FetchResult profileLast();
const char *profileParseNote();  // after FETCH_PARSE: "user IncompleteInput" etc.
uint32_t profileGeneration();  // changes whenever profileData() does
void profileStatusJson(Print &out);
