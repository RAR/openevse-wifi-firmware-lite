#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../src/lite/lite_clock.h"
#include <cstring>

TEST_CASE("nowUtc advances with millis from the synced epoch") {
  LiteClock c;
  CHECK_FALSE(c.valid());
  CHECK(c.nowUtc(123456) == 0u);          // invalid before first sync
  c.setEpoch(1735689600u, 1000u);         // 2025-01-01 00:00:00 UTC at ms=1000
  CHECK(c.valid());
  CHECK(c.nowUtc(1000u)    == 1735689600u);
  CHECK(c.nowUtc(31000u)   == 1735689630u); // +30 s
  CHECK(c.nowUtc(1000u + 3600000u) == 1735689600u + 3600u); // +1 h
}

TEST_CASE("nowUtc is correct across a 32-bit millis wrap") {
  LiteClock c;
  c.setEpoch(1735689600u, 0xFFFFF000u);   // synced just before wrap
  uint32_t afterWrap = 0xFFFFF000u + 5000u; // wraps past 2^32
  CHECK(c.nowUtc(afterWrap) == 1735689600u + 5u);
}

TEST_CASE("tz offset shifts local time only") {
  LiteClock c;
  c.setEpoch(1735689600u, 0u);
  c.setTzOffsetMinutes(-300);             // UTC-5
  CHECK(c.nowUtc(0u)   == 1735689600u);
  CHECK(c.nowLocal(0u) == 1735689600u - 300u * 60u);
  CHECK(c.tzOffsetMinutes() == -300);
}

TEST_CASE("resyncDue: always due before first sync, then after the interval") {
  LiteClock c;
  CHECK(c.resyncDue(0u));                  // never synced
  c.setEpoch(1735689600u, 1000u);
  CHECK_FALSE(c.resyncDue(1000u));
  CHECK_FALSE(c.resyncDue(1000u + LiteClock::RESYNC_INTERVAL_MS - 1u));
  CHECK(c.resyncDue(1000u + LiteClock::RESYNC_INTERVAL_MS));
}

TEST_CASE("civil_from_secs decodes known dates") {
  int y; unsigned m, d;
  lite_civil_from_secs(1735689600u, y, m, d);  // 2025-01-01
  CHECK(y == 2025); CHECK(m == 1u); CHECK(d == 1u);
  lite_civil_from_secs(1738368000u, y, m, d);  // 2025-02-01
  CHECK(y == 2025); CHECK(m == 2u); CHECK(d == 1u);
  lite_civil_from_secs(1767225600u, y, m, d);  // 2026-01-01
  CHECK(y == 2026); CHECK(m == 1u); CHECK(d == 1u);
  lite_civil_from_secs(1709164800u, y, m, d);  // 2024-02-29 (leap day)
  CHECK(y == 2024); CHECK(m == 2u); CHECK(d == 29u);
  lite_civil_from_secs(1735689599u, y, m, d);  // 2024-12-31 23:59:59 -> civil 2024-12-31
  CHECK(y == 2024); CHECK(m == 12u); CHECK(d == 31u);
}

TEST_CASE("iso8601 formats UTC epoch") {
  char buf[24];
  lite_clock_iso8601(1735689600u, buf, sizeof(buf));   // 2025-01-01 00:00:00
  CHECK(strcmp(buf, "2025-01-01T00:00:00Z") == 0);
  lite_clock_iso8601(1735689600u + 3661u, buf, sizeof(buf)); // +01:01:01
  CHECK(strcmp(buf, "2025-01-01T01:01:01Z") == 0);
  lite_clock_iso8601(1735775999u, buf, sizeof(buf));  // 2025-01-01 23:59:59
  CHECK(strcmp(buf, "2025-01-01T23:59:59Z") == 0);
}
