#include <cstdint>
#include "check.h"
#include "util.h"

static uint32_t counter() {
  static uint32_t n = 0;
  return n++;
}

int main() {
  // parseIso8601: the API's timestamps, UTC only.
  CHECK(parseIso8601("1970-01-01T00:00:00Z") == 0);
  CHECK(parseIso8601("2026-09-30T11:16:30.908Z") == 1790766990);
  CHECK(parseIso8601("2026-06-07T19:37:15.286Z") == 1780861035);
  CHECK(parseIso8601("2000-02-29T00:00:00Z") == 951782400);
  CHECK(parseIso8601("") == -1);
  CHECK(parseIso8601(nullptr) == -1);
  CHECK(parseIso8601("2026-09-30") == -1);
  CHECK(parseIso8601("2026-13-01T00:00:00Z") == -1);
  CHECK(parseIso8601("2026-09-30T11:16:30+02:00") == -1);

  char b[96];
  formatThousands(5520333, b, sizeof b); CHECK_STR(b, "5,520,333");
  formatThousands(0, b, sizeof b);       CHECK_STR(b, "0");
  formatThousands(999, b, sizeof b);     CHECK_STR(b, "999");
  formatThousands(1000, b, sizeof b);    CHECK_STR(b, "1,000");
  formatThousands(1331, b, sizeof b);    CHECK_STR(b, "1,331");
  formatThousands(-1234, b, sizeof b);   CHECK_STR(b, "-1,234");

  // Review Focus 3: an unset clock gives no age at all.
  const int64_t now = 1790766990;
  relativeAge(5, now, b, sizeof b);           CHECK_STR(b, "");
  relativeAge(now, 0, b, sizeof b);           CHECK_STR(b, "");
  relativeAge(now, -1, b, sizeof b);          CHECK_STR(b, "");
  relativeAge(now, now - 5, b, sizeof b);     CHECK_STR(b, "just now");
  relativeAge(now, now + 30, b, sizeof b);    CHECK_STR(b, "just now");
  relativeAge(now, now - 240, b, sizeof b);   CHECK_STR(b, "4m ago");
  relativeAge(now, now - 7200, b, sizeof b);  CHECK_STR(b, "2h ago");
  relativeAge(now, now - 3 * 86400, b, sizeof b); CHECK_STR(b, "3d ago");

  // Review Focus 1: the panel font is ASCII only.
  toDisplayAscii("model method \xC3\x97" "2", b, sizeof b);   CHECK_STR(b, "model method x2");
  toDisplayAscii("Marshlight \xC2\xB7 Prismatic", b, sizeof b); CHECK_STR(b, "Marshlight - Prismatic");
  toDisplayAscii("hi \xF0\x9F\x94\xA5", b, sizeof b);          CHECK_STR(b, "hi ?");
  toDisplayAscii("a\xE2\x80\xA6", b, sizeof b);                CHECK_STR(b, "a...");
  toDisplayAscii("bad \xC3", b, sizeof b);                     CHECK_STR(b, "bad ?");
  toDisplayAscii("tab\there", b, sizeof b);                    CHECK_STR(b, "tab here");
  toDisplayAscii(nullptr, b, sizeof b);                        CHECK_STR(b, "");
  char small[4];
  toDisplayAscii("abcdef", small, sizeof small);               CHECK_STR(small, "abc");
  toDisplayAscii("a\xE2\x80\xA6", small, sizeof small);        CHECK_STR(small, "a");
  // Accented letters lose the accent; ligatures and sharp s spell out.
  toDisplayAscii("Jos\xC3\xA9", b, sizeof b);                   CHECK_STR(b, "Jose");
  toDisplayAscii("Stra\xC3\x9F" "e", b, sizeof b);              CHECK_STR(b, "Strasse");
  toDisplayAscii("\xC5\x81\xC3\xB3" "d\xC5\xBA", b, sizeof b);  CHECK_STR(b, "Lodz");
  toDisplayAscii("\xC5\x92uvre", b, sizeof b);                  CHECK_STR(b, "OEuvre");
  toDisplayAscii("e\xCC\x81", b, sizeof b);                     CHECK_STR(b, "e");
  toDisplayAscii("a\xC2\xA0" "b", b, sizeof b);                 CHECK_STR(b, "a b");
  // One emoji is one "?", however many code points build it.
  toDisplayAscii("\xE2\x9D\xA4\xEF\xB8\x8F!", b, sizeof b);     CHECK_STR(b, "?!");
  toDisplayAscii("\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7 x", b,
                 sizeof b);                                     CHECK_STR(b, "? x");
  toDisplayAscii("\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD", b, sizeof b); CHECK_STR(b, "?");
  toDisplayAscii("\xF0\x9F\x87\xB3\xF0\x9F\x87\xB1", b, sizeof b); CHECK_STR(b, "?");
  toDisplayAscii("\xF0\x9F\x87\xB3\xF0\x9F\x87\xB1\xF0\x9F\x87\xA9\xF0\x9F\x87\xAA", b, sizeof b);
  CHECK_STR(b, "??");
  toDisplayAscii("1\xEF\xB8\x8F\xE2\x83\xA3", b, sizeof b);     CHECK_STR(b, "1");

  fitText("OG Swamper", 25, b, sizeof b);        CHECK_STR(b, "OG Swamper");
  fitText("abcdefghijklmnop", 10, b, sizeof b);  CHECK_STR(b, "abcdefg...");

  char l1[16], l2[16];
  wrap2("Quality Initiate", 10, l1, l2, sizeof l1);       CHECK_STR(l1, "Quality");    CHECK_STR(l2, "Initiate");
  wrap2("OG Swamper", 10, l1, l2, sizeof l1);             CHECK_STR(l1, "OG Swamper"); CHECK_STR(l2, "");
  wrap2("Supercalifragilistic", 10, l1, l2, sizeof l1);   CHECK_STR(l1, "Supercalif"); CHECK_STR(l2, "ragilistic");
  wrap2("Marshlight - Prismatic", 10, l1, l2, sizeof l1); CHECK_STR(l1, "Marshlight"); CHECK_STR(l2, "- Prism...");

  CHECK(validUsername("example"));
  CHECK(validUsername("ex.am_ple-1"));
  CHECK(validUsername("abcdefghijklmnopqrstuvwxyz0123456789abc"));    // 39
  CHECK(!validUsername("abcdefghijklmnopqrstuvwxyz0123456789abcd"));  // 40
  CHECK(!validUsername(""));
  CHECK(!validUsername("has space"));
  CHECK(!validUsername("\xC3\xBC"));
  CHECK(!validUsername("a/b"));

  // Review Focus 2: Wi-Fi QR strings escape \ ; , : and ".
  wifiQrText("swamp-ab12", "k3m9x2qa", b, sizeof b);
  CHECK_STR(b, "WIFI:T:WPA;S:swamp-ab12;P:k3m9x2qa;;");
  wifiQrText("a;b", "p:q\\\"", b, sizeof b);
  CHECK_STR(b, "WIFI:T:WPA;S:a\\;b;P:p\\:q\\\\\\\";;");

  htmlEscape("<a&b>\"'", b, sizeof b);
  CHECK_STR(b, "&lt;a&amp;b&gt;&quot;&#39;");

  // Why a Wi-Fi join failed, from ESP-IDF's wifi_err_reason_t.
  CHECK_STR(joinReasonText(0), "timeout");
  CHECK_STR(joinReasonText(201), "network not found");
  CHECK_STR(joinReasonText(202), "wrong password");
  CHECK_STR(joinReasonText(15), "wrong password");
  CHECK_STR(joinReasonText(204), "wrong password");
  CHECK_STR(joinReasonText(2), "wrong password");
  CHECK_STR(joinReasonText(203), "refused by the router");
  CHECK_STR(joinReasonText(205), "refused by the router");
  CHECK_STR(joinReasonText(8), "reason 8");

  // Touch debouncing: the controller drops out for a few ms mid-press.
  {
    TapFilter f;  // a clean 100 ms tap: one TAP once the finger has been up 120 ms
    CHECK(tapFilterFeed(f, true, 1000) == TOUCH_NONE);
    CHECK(tapFilterFeed(f, true, 1100) == TOUCH_NONE);
    CHECK(tapFilterFeed(f, false, 1150) == TOUCH_NONE);
    CHECK(tapFilterFeed(f, false, 1225) == TOUCH_TAP);
    CHECK(tapFilterFeed(f, false, 1400) == TOUCH_NONE);
  }
  {
    TapFilter f;  // one press with dropouts is still one tap
    int taps = 0;
    const bool pattern[] = {1, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1};  // 20 ms steps
    for (int i = 0; i < 11; i++) taps += tapFilterFeed(f, pattern[i], 2000 + i * 20) == TOUCH_TAP;
    for (uint32_t t = 2220; t < 2600; t += 20) taps += tapFilterFeed(f, false, t) == TOUCH_TAP;
    CHECK(taps == 1);
  }
  {
    TapFilter f;  // a 5 s hold with dropouts: one HOLD, and no tap on release
    int holds = 0, taps = 0;
    for (uint32_t t = 0; t <= 5200; t += 20) {
      TouchEvent e = tapFilterFeed(f, (t / 20) % 7 != 3, t);
      holds += e == TOUCH_HOLD;
      taps += e == TOUCH_TAP;
    }
    for (uint32_t t = 5220; t < 5600; t += 20) taps += tapFilterFeed(f, false, t) == TOUCH_TAP;
    CHECK(holds == 1);
    CHECK(taps == 0);
  }
  {
    TapFilter f;  // two real taps 400 ms apart are two taps
    int taps = 0;
    for (uint32_t t = 0; t < 1000; t += 20) {
      bool on = (t < 100) || (t >= 400 && t < 500);
      taps += tapFilterFeed(f, on, t) == TOUCH_TAP;
    }
    CHECK(taps == 2);
  }
  {
    TapFilter f;  // a 1.5 s press is neither a tap nor a hold
    int events = 0;
    for (uint32_t t = 0; t < 2000; t += 20) events += tapFilterFeed(f, t < 1500, t) != TOUCH_NONE;
    CHECK(events == 0);
  }

  // The setup form: a typed network name wins over the drop-down, exactly as typed.
  CHECK_STR(pickSsid("Strongest Net", ""), "Strongest Net");
  CHECK_STR(pickSsid("Strongest Net", "Hidden Net"), "Hidden Net");
  CHECK_STR(pickSsid("", "  spaced  name "), "  spaced  name ");
  CHECK_STR(pickSsid("", ""), "");

  // Which activity entries are new since the previous list (to highlight them).
  {
    Activity before[3] = {{"workflow validate x8", 1331, 300}, {"model method x2", 333, 200},
                          {"model method x1", 166, 100}};
    Activity now[3] = {{"model method x3", 499, 400}, {"workflow validate x8", 1331, 300},
                       {"model method x2", 333, 200}};
    bool isNew[3];
    newActivityMask(before, 3, now, 3, isNew);
    CHECK(isNew[0] && !isNew[1] && !isNew[2]);
    // Same title and time but different points is a different entry.
    Activity changed[1] = {{"model method x2", 999, 200}};
    newActivityMask(before, 3, changed, 1, isNew);
    CHECK(isNew[0]);
    // Nothing before (first load): nothing is highlighted.
    newActivityMask(before, 0, now, 3, isNew);
    CHECK(!isNew[0] && !isNew[1] && !isNew[2]);
  }

  randomPassword(counter, b, 8);
  CHECK_STR(b, "abcdefgh");
  randomPassword(counter, b, 31);
  CHECK(std::strlen(b) == 31);
  CHECK(std::strpbrk(b, "01ilo") == nullptr);

  return finish();
}
