#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../src/lite/lite_feed.h"

TEST_CASE("setters stamp value + time + validity") {
  LiteFeed f;
  CHECK_FALSE(f.solar_valid);
  lite_feed_set_solar(f, 1500, 12345);
  CHECK(f.solar_w == 1500); CHECK(f.solar_ms == 12345); CHECK(f.solar_valid);
  lite_feed_set_voltage(f, 241.5, 50);
  CHECK(f.voltage == doctest::Approx(241.5)); CHECK(f.voltage_valid);
}
TEST_CASE("freshness boundary + invalid") {
  CHECK_FALSE(lite_feed_fresh(false, 0, 0, 1000));      // never set -> stale
  CHECK(lite_feed_fresh(true, 0, 1000, 1000));          // exactly max age -> fresh (inclusive)
  CHECK_FALSE(lite_feed_fresh(true, 0, 1001, 1000));    // just past -> stale
  CHECK(lite_feed_fresh(true, 0, 999, 1000));           // within -> fresh
}
TEST_CASE("freshness wrap-safe across millis rollover") {
  uint32_t last = 0xFFFFFF00u, now = 0x00000064u;       // delta = 356
  CHECK(lite_feed_fresh(true, last, now, 500));         // 356 <= 500 -> fresh
  CHECK_FALSE(lite_feed_fresh(true, last, now, 200));   // 356 > 200 -> stale
}
