#include "profile.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <string.h>
#include <strings.h>

#include "config.h"
#include "net.h"
#include "profile_parse.h"

static Profile current;
static bool have = false;
static FetchResult last = FETCH_NONE;
static int lastCode = 0;
static char api[96];
static char user[40];
static unsigned long nextDueMs = 0;
static unsigned long lastOkMs = 0;
static unsigned long lastAttemptMs = 0;
static int backoffIdx = 0;
static bool stopped = false;  // after a 404, until the next profileBegin
static bool tapWanted = false;
static uint32_t generation = 0;
static char parseNote[40] = "";  // which request failed to parse, and how

static const unsigned long REFRESH_MS = 15UL * 60 * 1000;  // whole profile
static const unsigned long ACTIVITY_MS = 10UL * 1000;      // combat log only (~3 KB)
static const unsigned long STALE_ON_TAP_MS = 5UL * 60 * 1000;
static const unsigned long MIN_TAP_GAP_MS = 60UL * 1000;
static const unsigned long BACKOFF_MS[] = {60000UL, 120000UL, 300000UL, 900000UL};

static FetchResult getJson(const String &url, JsonDocument &filter, JsonDocument &doc, int &code,
                           char *note, size_t noteCap) {
  NetworkClientSecure client;
  client.useBuiltinCACertBundle();
  client.setHandshakeTimeout(10);  // seconds; the default is 120
  HTTPClient http;
  http.useHTTP10(true);  // no chunked encoding
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  http.setUserAgent("s3panel/0.12.2");
  if (!http.begin(client, url)) return FETCH_CONNECT;
  code = http.GET();
  FetchResult r;
  if (code <= 0) r = FETCH_CONNECT;
  else if (code == 404) r = FETCH_NOT_FOUND;
  else if (code != 200) r = FETCH_HTTP;
  else {
    // Read the whole body first: the response has no Content-Length, and
    // parsing straight from the TLS stream stops at the first pause
    // (IncompleteInput). ~50 KB, which lands in PSRAM.
    String body = http.getString();
    DeserializationError e = deserializeJson(doc, body, DeserializationOption::Filter(filter));
    strlcpy(note, e.c_str(), noteCap);
    r = e ? FETCH_PARSE : FETCH_OK;
  }
  http.end();
  return r;
}

// One fetch into caller-owned buffers, so the worker and a synchronous caller
// can never overwrite each other's work.
static FetchResult fetchInto(const char *apiBase, const char *username, Profile &out, int &code,
                             Profile &t, char *note, size_t noteCap) {
  memset(&t, 0, sizeof t);
  note[0] = '\0';
  String base = String(apiBase) + "/api/v1/users/" + username;
  {
    JsonDocument filter, doc;
    userFilter(filter);
    FetchResult r = getJson(base, filter, doc, code, note, noteCap);
    if (r == FETCH_PARSE) {
      char n[40];
      snprintf(n, sizeof n, "user %s", note);
      strlcpy(note, n, noteCap);
    }
    if (r != FETCH_OK) return r;
    if (!parseUser(doc, t)) {
      strlcpy(note, "user shape", noteCap);
      return FETCH_PARSE;
    }
  }
  {
    JsonDocument filter, doc;
    activityFilter(filter);
    FetchResult r = getJson(base + "/combat-log?limit=8", filter, doc, code, note, noteCap);
    if (r == FETCH_NOT_FOUND) {
      r = FETCH_OK;  // the user exists (above): no combat log is no activity
      t.activityCount = 0;
    } else {
      if (r == FETCH_PARSE) {
        char n[40];
        snprintf(n, sizeof n, "activity %s", note);
        strlcpy(note, n, noteCap);
      }
      if (r != FETCH_OK) return r;
      if (!parseActivity(doc, t)) {
        strlcpy(note, "activity shape", noteCap);
        return FETCH_PARSE;
      }
    }
  }
  t.fetchedAt = netNow();
  out = t;
  return FETCH_OK;
}

// Only the combat log, into t.activity (t is scratch). Cheap enough to run
// every ACTIVITY_MS; never cached, so frequent updates cost no flash wear.
static FetchResult fetchActivityInto(const char *apiBase, const char *username, int &code,
                                     Profile &t, char *note, size_t noteCap) {
  memset(&t, 0, sizeof t);
  String url = String(apiBase) + "/api/v1/users/" + username + "/combat-log?limit=8";
  JsonDocument filter, doc;
  activityFilter(filter);
  FetchResult r = getJson(url, filter, doc, code, note, noteCap);
  if (r != FETCH_OK) return r;
  return parseActivity(doc, t) ? FETCH_OK : FETCH_PARSE;
}

// Synchronous callers (loop task only: profile refresh, config set, setup).
FetchResult profileFetch(const char *apiBase, const char *username, Profile &out, int &code) {
  static Profile t;  // static: 2 KB off the loop task's stack
  return fetchInto(apiBase, username, out, code, t, parseNote, sizeof parseNote);
}

void profileAdopt(const Profile &p) {
  current = p;
  have = true;
  last = FETCH_OK;
  lastOkMs = millis();
  backoffIdx = 0;
  stopped = false;
  nextDueMs = millis() + REFRESH_MS;
  cacheSave(current);
  generation++;
}

// Scheduled fetches run on a worker task (core 0), because a fetch takes ~11 s
// on this board and the loop must keep answering USB and touch meanwhile. The
// worker only writes `bgOut`/`bgResult`/`bgCode` and then sets `bgReady`; the
// loop adopts the result if it is still for the current settings (epoch).
// Synchronous fetches (profileRefresh, config set, setup) first wait for the
// worker to be idle, since profileFetch's buffers are shared.
static TaskHandle_t worker = nullptr;
static Profile bgScratch;  // the worker's own buffers
static char bgNote[40];
static volatile bool bgActivityOnly = false;  // the job the worker is on
static unsigned long nextActivityMs = 0;
static volatile bool bgBusy = false;
static volatile bool bgReady = false;
static Profile bgOut;
static FetchResult bgResult = FETCH_NONE;
static int bgCode = 0;
static char bgApi[96], bgUser[40];  // the worker's own copies
static uint32_t epoch = 0;          // bumped by profileBegin: older results are dropped
static volatile uint32_t bgEpoch = 0;

static void workerTask(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (bgActivityOnly) {
      bgResult = fetchActivityInto(bgApi, bgUser, bgCode, bgScratch, bgNote, sizeof bgNote);
      if (bgResult == FETCH_OK) bgOut = bgScratch;  // only .activity is meaningful
    } else {
      bgResult = fetchInto(bgApi, bgUser, bgOut, bgCode, bgScratch, bgNote, sizeof bgNote);
    }
    bgReady = true;
    bgBusy = false;
  }
}

bool profileWaitIdle(uint32_t timeoutMs) {
  unsigned long t0 = millis();
  while (bgBusy && millis() - t0 < timeoutMs) delay(20);
  return !bgBusy;
}

void profileBegin(const char *apiBase, const char *username) {
  if (!worker) xTaskCreatePinnedToCore(workerTask, "profile", 16384, nullptr, 1, &worker, 0);
  epoch++;  // a fetch still running for the old settings is discarded
  strlcpy(api, apiBase, sizeof api);
  strlcpy(user, username, sizeof user);
  memset(&current, 0, sizeof current);
  have = user[0] && cacheLoad(current) && strcasecmp(current.username, user) == 0;
  if (!have) memset(&current, 0, sizeof current);
  last = FETCH_NONE;
  lastOkMs = 0;
  backoffIdx = 0;
  stopped = false;
  nextDueMs = millis();
  generation++;
}

// What a finished fetch means for the schedule, whichever way it ran.
static void settle(FetchResult r, const Profile &fresh, int code) {
  lastCode = code;
  if (r == FETCH_OK) {
    profileAdopt(fresh);
    return;
  }
  last = r;
  if (r == FETCH_NOT_FOUND) {
    stopped = true;
  } else {
    nextDueMs = millis() + BACKOFF_MS[backoffIdx];
    if (backoffIdx < 3) backoffIdx++;
  }
  generation++;  // the footer note changes
}

void profileLoop(bool wifiUp, bool mayFetch) {
  if (bgReady) {
    bgReady = false;
    if (bgEpoch != epoch) {
      // settings changed while it ran: drop it
    } else if (bgActivityOnly) {
      // Merge new activity into what the screens show; failures are silent
      // (the next full fetch reports real problems).
      if (bgResult == FETCH_OK && have &&
          (bgOut.activityCount != current.activityCount ||
           memcmp(bgOut.activity, current.activity, sizeof current.activity) != 0)) {
        current.activityCount = bgOut.activityCount;
        memcpy(current.activity, bgOut.activity, sizeof current.activity);
        generation++;
      }
    } else {
      settle(bgResult, bgOut, bgCode);
    }
  }
  // Wi-Fi back after a failed fetch: try now instead of waiting out the backoff,
  // so "offline" does not outlive the outage.
  static bool wasUp = false;
  if (wifiUp && !wasUp && (last == FETCH_CONNECT || last == FETCH_HTTP)) {
    nextDueMs = millis();
    backoffIdx = 0;
  }
  wasUp = wifiUp;
  if (!user[0] || stopped || !wifiUp || !mayFetch || bgBusy || !worker) return;
  unsigned long now = millis();
  bool due = (long)(now - nextDueMs) >= 0;
  if (tapWanted) {
    tapWanted = false;
    if ((!have || now - lastOkMs > STALE_ON_TAP_MS) && now - lastAttemptMs > MIN_TAP_GAP_MS) due = true;
  }
  bool activityDue = have && (long)(now - nextActivityMs) >= 0;
  if (!due && !activityDue) return;
  bgActivityOnly = !due;
  nextActivityMs = now + ACTIVITY_MS;
  if (due) {
    lastAttemptMs = now;
    nextDueMs = now + REFRESH_MS;  // settle() sets the real next time
  }
  strlcpy(bgApi, api, sizeof bgApi);
  strlcpy(bgUser, user, sizeof bgUser);
  bgEpoch = epoch;
  bgBusy = true;
  xTaskNotifyGive(worker);
}

FetchResult profileRefresh() {
  if (!user[0]) {
    last = FETCH_NONE;
    return last;
  }
  if (!netConnected()) {
    last = FETCH_CONNECT;
    generation++;
    return last;
  }
  profileWaitIdle(30000);
  if (bgReady) {  // a background fetch just finished: take it first
    bgReady = false;
    if (bgEpoch == epoch && !bgActivityOnly) settle(bgResult, bgOut, bgCode);  // an activity-only result is superseded by this fetch
  }
  static Profile next;
  int code = 0;
  lastAttemptMs = millis();
  FetchResult r = profileFetch(api, user, next, code);
  settle(r, next, code);
  return r;
}

void profileNoteTap() { tapWanted = true; }
bool profileHave() { return have; }
const Profile &profileData() { return current; }
const char *profileUsername() { return user; }
FetchResult profileLast() { return last; }
uint32_t profileGeneration() { return generation; }
const char *profileParseNote() { return parseNote; }

void profileStatusJson(Print &out) {
  char err[16] = "";
  switch (last) {
    case FETCH_NOT_FOUND: strcpy(err, "not found"); break;
    case FETCH_CONNECT: strcpy(err, "connect"); break;
    case FETCH_HTTP: snprintf(err, sizeof err, "http %d", lastCode); break;
    case FETCH_PARSE: strcpy(err, "parse"); break;
    default: break;
  }
  JsonDocument d;
  d["ok"] = last == FETCH_OK;
  if (have && current.fetchedAt > 0) d["fetchedAt"] = current.fetchedAt;
  else d["fetchedAt"] = nullptr;
  if (err[0]) d["error"] = err;
  else d["error"] = nullptr;
  d["username"] = have ? current.username : user;
  d["points"] = current.points;
  d["rank"] = current.rank;
  d["tier"] = current.tier;
  d["badges"] = current.badgeTotal;
  d["activity"] = current.activityCount;
  d["cached"] = have;
  serializeJson(d, out);
  out.println();
}
