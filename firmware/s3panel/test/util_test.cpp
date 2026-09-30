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

  randomPassword(counter, b, 8);
  CHECK_STR(b, "abcdefgh");
  randomPassword(counter, b, 31);
  CHECK(std::strlen(b) == 31);
  CHECK(std::strpbrk(b, "01ilo") == nullptr);

  return finish();
}
