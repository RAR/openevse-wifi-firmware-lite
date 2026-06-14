#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../src/lite/lite_evse_properties.h"

TEST_CASE("defaults are unset / None") {
  EvseProperties p;
  CHECK(p.getState() == EvseState::None);
  CHECK(p.hasChargeCurrent() == false);
  CHECK(p.hasMaxCurrent() == false);
  CHECK(p.isAutoRelease() == false);
  CHECK(p.hasAutoRelease() == false);
}

TEST_CASE("state constructor") {
  EvseProperties p(EvseState::Disabled);
  CHECK(p.getState() == EvseState::Disabled);
  CHECK(p.hasChargeCurrent() == false);
}

TEST_CASE("setters flip has-flags") {
  EvseProperties p;
  p.setChargeCurrent(16);
  CHECK(p.hasChargeCurrent() == true);
  CHECK(p.getChargeCurrent() == 16u);
  p.setMaxCurrent(32);
  CHECK(p.hasMaxCurrent() == true);
  CHECK(p.getMaxCurrent() == 32u);
  p.setAutoRelease(true);
  CHECK(p.isAutoRelease() == true);
  CHECK(p.hasAutoRelease() == true);
}

TEST_CASE("clear resets to unset") {
  EvseProperties p(EvseState::Active);
  p.setChargeCurrent(10);
  p.clear();
  CHECK(p.getState() == EvseState::None);
  CHECK(p.hasChargeCurrent() == false);
}

TEST_CASE("assignment copies all fields") {
  EvseProperties a(EvseState::Active);
  a.setChargeCurrent(20);
  EvseProperties b;
  b = a;
  CHECK(b.getState() == EvseState::Active);
  CHECK(b.getChargeCurrent() == 20u);
}
