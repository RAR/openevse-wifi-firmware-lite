#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../src/lite/lite_led.h"

static bool colEq(LiteLedColor c, bool r, bool g, bool b) {
  return c.r == r && c.g == g && c.b == b;
}

TEST_CASE("offline outranks everything -> white slow-blink") {
  LiteLedSpec s = lite_led_for(LiteEvseState::Error, true, false); // error+disabled+offline
  CHECK(colEq(s.color, 1, 1, 1));
  CHECK(s.pattern == LiteLedPattern::SlowBlink);
}

TEST_CASE("error (online) -> red fast-blink, outranks charging/disabled") {
  LiteLedSpec s = lite_led_for(LiteEvseState::Error, true, true);
  CHECK(colEq(s.color, 1, 0, 0));
  CHECK(s.pattern == LiteLedPattern::FastBlink);
}

TEST_CASE("policy-disabled outranks device sub-state -> yellow solid") {
  CHECK(colEq(lite_led_for(LiteEvseState::Connected, true, true).color, 1, 1, 0));
  CHECK(colEq(lite_led_for(LiteEvseState::Charging,  true, true).color, 1, 1, 0)); // even charging
  CHECK(lite_led_for(LiteEvseState::Connected, true, true).pattern == LiteLedPattern::Solid);
}

TEST_CASE("charging -> green slow-blink") {
  LiteLedSpec s = lite_led_for(LiteEvseState::Charging, false, true);
  CHECK(colEq(s.color, 0, 1, 0));
  CHECK(s.pattern == LiteLedPattern::SlowBlink);
}

TEST_CASE("connected -> cyan solid; not_connected -> blue solid") {
  CHECK(colEq(lite_led_for(LiteEvseState::Connected,    false, true).color, 0, 1, 1));
  CHECK(colEq(lite_led_for(LiteEvseState::NotConnected, false, true).color, 0, 0, 1));
  CHECK(lite_led_for(LiteEvseState::NotConnected, false, true).pattern == LiteLedPattern::Solid);
}

TEST_CASE("unknown (online) -> white solid") {
  LiteLedSpec s = lite_led_for(LiteEvseState::Unknown, false, true);
  CHECK(colEq(s.color, 1, 1, 1));
  CHECK(s.pattern == LiteLedPattern::Solid);
}

TEST_CASE("phase_on: solid always on") {
  CHECK(lite_led_phase_on(LiteLedPattern::Solid, 0));
  CHECK(lite_led_phase_on(LiteLedPattern::Solid, 1234567));
}

TEST_CASE("phase_on: slow blink ~1Hz (500ms half)") {
  CHECK(lite_led_phase_on(LiteLedPattern::SlowBlink, 0));        // on
  CHECK(lite_led_phase_on(LiteLedPattern::SlowBlink, 499));      // on
  CHECK_FALSE(lite_led_phase_on(LiteLedPattern::SlowBlink, 500)); // off
  CHECK_FALSE(lite_led_phase_on(LiteLedPattern::SlowBlink, 999)); // off
  CHECK(lite_led_phase_on(LiteLedPattern::SlowBlink, 1000));     // on again
}

TEST_CASE("phase_on: fast blink toggles at 160ms") {
  CHECK(lite_led_phase_on(LiteLedPattern::FastBlink, 0));
  CHECK_FALSE(lite_led_phase_on(LiteLedPattern::FastBlink, 160));
  CHECK(lite_led_phase_on(LiteLedPattern::FastBlink, 320));
}
