#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../src/lite/lite_override.h"

static LiteOverrideLimits energyLimit(uint32_t wh) {
  LiteOverrideLimits l; l.energy_limit_wh = wh; l.has_energy = true; return l;
}
static LiteOverrideLimits timeLimit(uint32_t s) {
  LiteOverrideLimits l; l.time_limit_s = s; l.has_time = true; return l;
}

TEST_CASE("inactive override never acts") {
  CHECK(lite_override_evaluate(energyLimit(100), 999, 0, false, true, true, true)
        == LiteOverrideAction::None);
}

TEST_CASE("no limits, no edge -> None") {
  LiteOverrideLimits none;
  CHECK(lite_override_evaluate(none, 50000, 99999, true, true, true, false)
        == LiteOverrideAction::None);
}

TEST_CASE("energy limit reached -> Stop (>= boundary)") {
  CHECK(lite_override_evaluate(energyLimit(100), 100, 0, true, true, false, false)
        == LiteOverrideAction::Stop);
  CHECK(lite_override_evaluate(energyLimit(100), 99, 0, true, true, false, false)
        == LiteOverrideAction::None);
}

TEST_CASE("time limit reached -> Stop (>= boundary)") {
  CHECK(lite_override_evaluate(timeLimit(3600), 0, 3600, true, true, false, false)
        == LiteOverrideAction::Stop);
  CHECK(lite_override_evaluate(timeLimit(3600), 0, 3599, true, true, false, false)
        == LiteOverrideAction::None);
}

TEST_CASE("Active auto-release on charge->idle edge -> Release") {
  LiteOverrideLimits none;
  CHECK(lite_override_evaluate(none, 0, 0, true, true, true, true)
        == LiteOverrideAction::Release);
  // no edge -> None
  CHECK(lite_override_evaluate(none, 0, 0, true, true, true, false)
        == LiteOverrideAction::None);
  // auto_release false -> None even on edge
  CHECK(lite_override_evaluate(none, 0, 0, true, true, false, true)
        == LiteOverrideAction::None);
  // not enabling (Disabled) + edge -> None (sticky, never released)
  CHECK(lite_override_evaluate(none, 0, 0, true, false, true, true)
        == LiteOverrideAction::None);
}

TEST_CASE("limit Stop outranks auto-release Release") {
  // limit exceeded AND Active+auto_release+edge -> Stop wins
  CHECK(lite_override_evaluate(energyLimit(100), 100, 0, true, true, true, true)
        == LiteOverrideAction::Stop);
}
