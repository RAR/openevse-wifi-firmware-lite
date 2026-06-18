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
TEST_CASE("HA push fields: absent until set, then stamp value+time+validity") {
  LiteFeed f;
  CHECK_FALSE(f.veh_soc_valid);
  CHECK_FALSE(f.veh_range_valid);
  CHECK_FALSE(f.hbatt_soc_valid);

  // 0% is legitimate data: validity flips on set even though the value is 0.
  lite_feed_set_vehicle_soc(f, 0, 100);
  CHECK(f.veh_soc == 0); CHECK(f.veh_soc_ms == 100); CHECK(f.veh_soc_valid);

  lite_feed_set_vehicle_range(f, 240, 200);
  CHECK(f.veh_range == 240); CHECK(f.veh_range_valid);
  lite_feed_set_vehicle_eta(f, 45, 300);
  CHECK(f.veh_eta == 45); CHECK(f.veh_eta_valid);
  lite_feed_set_home_battery_soc(f, 80, 400);
  CHECK(f.hbatt_soc == 80); CHECK(f.hbatt_soc_valid);
  lite_feed_set_home_battery_power(f, -1200, 500);   // discharging = negative
  CHECK(f.hbatt_pwr == -1200); CHECK(f.hbatt_pwr_valid);
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
