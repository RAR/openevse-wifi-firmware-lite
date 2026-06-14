#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../src/lite/lite_charge_policy.h"

TEST_CASE("service-max clamps to [6, 48]") {
  CHECK(lite_clamp_service_max(32) == 32);   // in range, unchanged
  CHECK(lite_clamp_service_max(3)  == 6);    // below floor -> 6
  CHECK(lite_clamp_service_max(0)  == 6);    // zero -> 6
  CHECK(lite_clamp_service_max(80) == 48);   // above cap -> 48
  CHECK(lite_clamp_service_max(6)  == 6);    // boundary
  CHECK(lite_clamp_service_max(48) == 48);   // boundary
}

TEST_CASE("charge current clamps to [6, clamped service-max]") {
  CHECK(lite_clamp_charge_current(20, 32) == 20);  // soft within hard
  CHECK(lite_clamp_charge_current(40, 24) == 24);  // soft above hard -> hard
  CHECK(lite_clamp_charge_current(3,  32) == 6);   // soft below floor -> 6
  CHECK(lite_clamp_charge_current(40, 80) == 40);  // hard over-cap clamps to 48 first, 40 fits
  CHECK(lite_clamp_charge_current(60, 80) == 48);  // both over -> 48
  CHECK(lite_clamp_charge_current(20, 3)  == 6);   // hard below floor -> service-max 6 -> soft 6
}
