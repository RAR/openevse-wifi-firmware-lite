#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../src/lite/lite_temp_throttle.h"

// setpoint 65 C, floor 6 A
static LiteTempThrottleCfg cfg() { return LiteTempThrottleCfg{ 65, 6 }; }

TEST_CASE("disabled with no prior throttle -> None, state stays idle") {
  LiteTempThrottleState st{ 0, 0 };
  auto r = lite_temp_throttle_tick(cfg(), st, false, true, 80, true, 32);
  CHECK(r.action == LiteTempThrottleAction::None);
  CHECK(st.start_current == 0);
}

TEST_CASE("disabled while throttling -> Release + reset") {
  LiteTempThrottleState st{ 32, 20 };
  auto r = lite_temp_throttle_tick(cfg(), st, false, true, 80, true, 32);
  CHECK(r.action == LiteTempThrottleAction::Release);
  CHECK(st.start_current == 0);
  CHECK(st.throttled_current == 0);
}

TEST_CASE("invalid temperature -> None") {
  LiteTempThrottleState st{ 0, 0 };
  auto r = lite_temp_throttle_tick(cfg(), st, true, false, 80, true, 32);
  CHECK(r.action == LiteTempThrottleAction::None);
}

TEST_CASE("at setpoint but not charging -> no engage") {
  LiteTempThrottleState st{ 0, 0 };
  auto r = lite_temp_throttle_tick(cfg(), st, true, true, 70, false, 32);
  CHECK(r.action == LiteTempThrottleAction::None);
  CHECK(st.start_current == 0);
}

TEST_CASE("at setpoint with zero pilot -> no engage") {
  LiteTempThrottleState st{ 0, 0 };
  auto r = lite_temp_throttle_tick(cfg(), st, true, true, 70, true, 0);
  CHECK(r.action == LiteTempThrottleAction::None);
  CHECK(st.start_current == 0);
}

TEST_CASE("engage at setpoint: capture start, drop 1 A, Claim") {
  LiteTempThrottleState st{ 0, 0 };
  auto r = lite_temp_throttle_tick(cfg(), st, true, true, 65, true, 32);
  CHECK(r.action == LiteTempThrottleAction::Claim);
  CHECK(st.start_current == 32);
  CHECK(st.throttled_current == 31);
  CHECK(r.charge_current == 31);
}

TEST_CASE("keeps ramping down toward the floor, never below it") {
  LiteTempThrottleState st{ 8, 7 };
  auto r = lite_temp_throttle_tick(cfg(), st, true, true, 70, true, 7);
  CHECK(r.action == LiteTempThrottleAction::Claim);
  CHECK(st.throttled_current == 6);   // floor
  // already at floor: stays
  r = lite_temp_throttle_tick(cfg(), st, true, true, 70, true, 6);
  CHECK(r.action == LiteTempThrottleAction::Claim);
  CHECK(st.throttled_current == 6);
}

TEST_CASE("recover below setpoint: ramp up 1 A, Claim") {
  LiteTempThrottleState st{ 32, 20 };
  auto r = lite_temp_throttle_tick(cfg(), st, true, true, 60, true, 20);
  CHECK(r.action == LiteTempThrottleAction::Claim);
  CHECK(st.throttled_current == 21);
}

TEST_CASE("fully recovered -> Release + reset") {
  LiteTempThrottleState st{ 32, 32 };
  auto r = lite_temp_throttle_tick(cfg(), st, true, true, 60, true, 32);
  CHECK(r.action == LiteTempThrottleAction::Release);
  CHECK(st.start_current == 0);
  CHECK(st.throttled_current == 0);
}

TEST_CASE("below setpoint while idle -> None") {
  LiteTempThrottleState st{ 0, 0 };
  auto r = lite_temp_throttle_tick(cfg(), st, true, true, 50, true, 32);
  CHECK(r.action == LiteTempThrottleAction::None);
}
