#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../src/lite/lite_led.h"

// lite_led_for(dev, disabled, commsOnline, apMode, wifiConnected) — OEM JuiceBox mimic.
// Colors are 0..255 per-channel duties (PWM); lite_led_envelope() is the brightness curve.

static bool colEq(LiteLedColor c, uint8_t r, uint8_t g, uint8_t b) {
  return c.r == r && c.g == g && c.b == b;
}

TEST_CASE("powering on: no controller comms -> orange breathe, outranks everything") {
  // even with apMode + error + disabled all set, !commsOnline wins
  LiteLedSpec s = lite_led_for(LiteEvseState::Error, true, /*commsOnline=*/false, true, false);
  CHECK(colEq(s.color, 255, 70, 0));              // true orange = red full + green low
  CHECK(s.pattern == LiteLedPattern::Breathe);
}

TEST_CASE("setup mode: softAP -> blue slow-blink (outranks error/charge)") {
  LiteLedSpec s = lite_led_for(LiteEvseState::Error, true, true, /*apMode=*/true, false);
  CHECK(colEq(s.color, 0, 0, 255));
  CHECK(s.pattern == LiteLedPattern::SlowBlink);
}

TEST_CASE("error -> red fast-blink (outranks disabled/charging)") {
  LiteLedSpec s = lite_led_for(LiteEvseState::Error, true, true, false, true);
  CHECK(colEq(s.color, 255, 0, 0));
  CHECK(s.pattern == LiteLedPattern::FastBlink);
}

TEST_CASE("disabled + vehicle present -> yellow flash (suspended), even mid-charge") {
  CHECK(colEq(lite_led_for(LiteEvseState::Connected, true, true, false, true).color, 255, 255, 0));
  CHECK(colEq(lite_led_for(LiteEvseState::Charging,  true, true, false, true).color, 255, 255, 0));
  CHECK(lite_led_for(LiteEvseState::Connected, true, true, false, true).pattern == LiteLedPattern::SlowBlink);
}

TEST_CASE("disabled + no vehicle -> red solid (standby, not ready)") {
  LiteLedSpec s = lite_led_for(LiteEvseState::NotConnected, true, true, false, true);
  CHECK(colEq(s.color, 255, 0, 0));
  CHECK(s.pattern == LiteLedPattern::Solid);
}

TEST_CASE("charging -> green slow-blink") {
  LiteLedSpec s = lite_led_for(LiteEvseState::Charging, false, true, false, true);
  CHECK(colEq(s.color, 0, 255, 0));
  CHECK(s.pattern == LiteLedPattern::SlowBlink);
}

TEST_CASE("ready online (WiFi up): plugged -> white solid; no vehicle -> white slow-blink") {
  LiteLedSpec plugged = lite_led_for(LiteEvseState::Connected, false, true, false, /*wifi=*/true);
  CHECK(colEq(plugged.color, 255, 255, 255));
  CHECK(plugged.pattern == LiteLedPattern::Solid);
  LiteLedSpec none = lite_led_for(LiteEvseState::NotConnected, false, true, false, true);
  CHECK(colEq(none.color, 255, 255, 255));
  CHECK(none.pattern == LiteLedPattern::SlowBlink);
}

TEST_CASE("ready offline (WiFi down): plugged -> purple solid; no vehicle -> purple slow-blink") {
  LiteLedSpec plugged = lite_led_for(LiteEvseState::Connected, false, true, false, /*wifi=*/false);
  CHECK(colEq(plugged.color, 255, 0, 255));       // purple = R+B
  CHECK(plugged.pattern == LiteLedPattern::Solid);
  LiteLedSpec none = lite_led_for(LiteEvseState::NotConnected, false, true, false, false);
  CHECK(colEq(none.color, 255, 0, 255));
  CHECK(none.pattern == LiteLedPattern::SlowBlink);
}

TEST_CASE("envelope: solid always full on (255)") {
  CHECK(lite_led_envelope(LiteLedPattern::Solid, 0) == 255);
  CHECK(lite_led_envelope(LiteLedPattern::Solid, 1234567) == 255);
}

TEST_CASE("envelope: slow blink (LITE_LED_SLOWBLINK_MS half), full on/off") {
  const uint32_t H = LITE_LED_SLOWBLINK_MS;
  CHECK(lite_led_envelope(LiteLedPattern::SlowBlink, 0)         == 255);  // on
  CHECK(lite_led_envelope(LiteLedPattern::SlowBlink, H - 1)     == 255);  // on
  CHECK(lite_led_envelope(LiteLedPattern::SlowBlink, H)         == 0);    // off
  CHECK(lite_led_envelope(LiteLedPattern::SlowBlink, 2 * H - 1) == 0);    // off
  CHECK(lite_led_envelope(LiteLedPattern::SlowBlink, 2 * H)     == 255);  // on again
}

TEST_CASE("envelope: fast blink toggles at LITE_LED_FASTBLINK_MS") {
  const uint32_t H = LITE_LED_FASTBLINK_MS;
  CHECK(lite_led_envelope(LiteLedPattern::FastBlink, 0)       == 255);
  CHECK(lite_led_envelope(LiteLedPattern::FastBlink, H)       == 0);
  CHECK(lite_led_envelope(LiteLedPattern::FastBlink, 2 * H)   == 255);
}

TEST_CASE("envelope: breathe is a gamma-corrected fade (0 at trough, 255 at peak)") {
  CHECK(lite_led_envelope(LiteLedPattern::Breathe, 0) == 0);                            // trough
  CHECK(lite_led_envelope(LiteLedPattern::Breathe, LITE_LED_BREATHE_MS / 2) == 255);    // peak
  // Quarter cycle: perceived ~0.5, but gamma (^2.2) pulls duty well below the linear
  // midpoint (~128) — proof the curve is pre-distorted, not linear.
  uint8_t quarter = lite_led_envelope(LiteLedPattern::Breathe, LITE_LED_BREATHE_MS / 4);
  CHECK(quarter > 0);
  CHECK(quarter < 128);
  // Monotonic rise across the first half (trough -> peak).
  CHECK(lite_led_envelope(LiteLedPattern::Breathe, LITE_LED_BREATHE_MS / 8) <
        lite_led_envelope(LiteLedPattern::Breathe, 3 * LITE_LED_BREATHE_MS / 8));
}
