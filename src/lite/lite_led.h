#pragma once
#include <stdint.h>
#include "lite_evse_state.h"

// Per-channel target intensity (0..255 duty), driven onto the RGB LED via PWM
// (analogWrite). Was bool on the old on/off driver; widened to uint8_t so "powering
// on" can render true orange (red full + green low) and Breathe can fade — neither of
// which a binary R/G/B LED could show.
struct LiteLedColor { uint8_t r, g, b; };

// Solid / blink rates, plus Breathe for the OEM "powering on" pulse — now a real
// gamma-corrected fade under the PWM driver (see lite_led_envelope).
enum class LiteLedPattern : uint8_t { Solid, SlowBlink, FastBlink, Breathe };
struct LiteLedSpec { LiteLedColor color; LiteLedPattern pattern; };

// One full Breathe cycle (dim -> bright -> dim), ms. Exposed so the device driver and
// the host tests share the exact period.
static constexpr uint32_t LITE_LED_BREATHE_MS = 2600;

// Resolve the indicator, mimicking the OEM JuiceBox LED guide
// (effortlesselectric.com/docs/juicebox-led-indicator). Priority (first match wins):
//   powering-on (no controller comms) > setup (softAP) > error > suspended/standby
//   (policy-disabled) > charging > ready (vehicle plugged / not).
// Args: `disabled` = manager target Disabled (manual Off / tripped limit); `commsOnline` =
// backend hearing the ATmega; `apMode` = WiFi-provisioning softAP; `wifiConnected` = STA
// joined. The white(online)/purple(offline) split applies ONLY to the "ready" states, per
// the OEM table. Colors are full-on per-channel duties; the pattern envelope scales them. Pure.
LiteLedSpec lite_led_for(LiteEvseState dev, bool disabled, bool commsOnline,
                         bool apMode, bool wifiConnected);

// Pattern brightness at nowMs as a 0..255 multiplier applied to every channel:
//   Solid -> 255; SlowBlink/FastBlink -> 255 or 0; Breathe -> a gamma(2.2)-corrected
// raised cosine so *perceived* brightness (not raw duty) sweeps a smooth sinusoid, with
// no snap at the bright end. Pure.
uint8_t lite_led_envelope(LiteLedPattern pattern, uint32_t nowMs);
