#include "lite_led.h"

LiteLedSpec lite_led_for(LiteEvseState dev, bool disabled, bool commsOnline,
                         bool apMode, bool wifiConnected) {
  // OEM JuiceBox LED mimic — effortlesselectric.com/docs/juicebox-led-indicator.
  // Evaluated top-down; first match wins. Colors are full-on per-channel duties
  // (0..255); lite_led_envelope() scales them for blink/breathe.

  // Powering on: controller comms not yet established (boot; also a sustained comms
  // drop). OEM is orange pulsing — true orange is red full + green low (the green tune
  // lands here, 50..90 is the usable range), breathing via the Breathe envelope.
  if (!commsOnline)                   return { {255, 70, 0}, LiteLedPattern::Breathe };

  // Setup mode: WiFi-provisioning softAP -> blue flashing.
  if (apMode)                         return { {0, 0, 255}, LiteLedPattern::SlowBlink };

  // Fault -> red fast-blink (urgent; outranks charge state).
  if (dev == LiteEvseState::Error)    return { {255, 0, 0}, LiteLedPattern::FastBlink };

  // Charge gated off by policy (manual Off / tripped limit). Vehicle present (Connected, or
  // still Charging mid ramp-down) -> yellow flashing "plugged in, ended/suspended"; no
  // vehicle -> red solid "standby, not ready". Checked before Charging so a just-disabled
  // car (still reporting Charging) reads suspended, not green.
  if (disabled) {
    if (dev == LiteEvseState::Connected || dev == LiteEvseState::Charging)
      return { {255, 255, 0}, LiteLedPattern::SlowBlink };   // yellow flash (suspended)
    return { {255, 0, 0}, LiteLedPattern::Solid };           // red solid (standby)
  }

  // Charging -> green flashing.
  if (dev == LiteEvseState::Charging) return { {0, 255, 0}, LiteLedPattern::SlowBlink };

  // Ready: color carries the WiFi link (white online / purple offline), pattern carries
  // vehicle presence (solid = plugged, flashing = no vehicle). Mirrors the OEM white/purple
  // "ready to charge" rows — that online/offline split shows ONLY in these ready states.
  const LiteLedColor   c = wifiConnected ? LiteLedColor{255, 255, 255}   // white
                                         : LiteLedColor{255, 0, 255};    // purple
  const LiteLedPattern p = (dev == LiteEvseState::Connected) ? LiteLedPattern::Solid
                                                             : LiteLedPattern::SlowBlink;
  return { c, p };
}

// generated: 128-entry gamma(2.2)-corrected raised-cosine breathe curve, one full
// period; duty so PERCEIVED brightness (duty^(1/2.2)) sweeps a smooth cosine. Baked at
// build time so the loop needs no libm/FPU per frame and the curve is deterministic.
static const uint8_t kBreatheLut[128] = {
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   1,   1,   1,   2,   2,   3,
    4,   5,   6,   8,   9,  11,  14,  16,  19,  22,  26,  30,  34,  39,  44,  50,
   55,  62,  68,  75,  82,  90,  97, 105, 113, 121, 130, 138, 147, 155, 164, 172,
  180, 188, 196, 203, 210, 217, 223, 229, 234, 239, 243, 247, 250, 252, 254, 255,
  255, 255, 254, 252, 250, 247, 243, 239, 234, 229, 223, 217, 210, 203, 196, 188,
  180, 172, 164, 155, 147, 138, 130, 121, 113, 105,  97,  90,  82,  75,  68,  62,
   55,  50,  44,  39,  34,  30,  26,  22,  19,  16,  14,  11,   9,   8,   6,   5,
    4,   3,   2,   2,   1,   1,   1,   0,   0,   0,   0,   0,   0,   0,   0,   0,
};

uint8_t lite_led_envelope(LiteLedPattern pattern, uint32_t nowMs) {
  switch (pattern) {
    case LiteLedPattern::SlowBlink: return (((nowMs / 500u) & 1u) == 0u) ? 255 : 0;  // ~1 Hz
    case LiteLedPattern::FastBlink: return (((nowMs / 160u) & 1u) == 0u) ? 255 : 0;  // ~3 Hz
    case LiteLedPattern::Breathe: {
      uint32_t n   = sizeof(kBreatheLut) / sizeof(kBreatheLut[0]);  // 128
      uint32_t idx = (nowMs % LITE_LED_BREATHE_MS) * n / LITE_LED_BREATHE_MS;  // 0..n-1
      return kBreatheLut[idx];
    }
    case LiteLedPattern::Solid:
    default:                        return 255;
  }
}
