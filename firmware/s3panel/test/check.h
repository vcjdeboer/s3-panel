// Minimal assertions for the host-side firmware tests (no framework needed).
#pragma once
#include <cstdio>
#include <cstring>

static int failures = 0;

#define CHECK(c)                                                    \
  do {                                                              \
    if (!(c)) {                                                     \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);      \
      failures++;                                                   \
    }                                                               \
  } while (0)

#define CHECK_STR(got, want)                                        \
  do {                                                              \
    const char *g_ = (got), *w_ = (want);                           \
    if (std::strcmp(g_, w_) != 0) {                                 \
      std::printf("FAIL %s:%d  got \"%s\" want \"%s\"\n", __FILE__, \
                  __LINE__, g_, w_);                                \
      failures++;                                                   \
    }                                                               \
  } while (0)

static int finish() {
  if (failures) {
    std::printf("%d FAILED\n", failures);
    return 1;
  }
  std::printf("all passed\n");
  return 0;
}
