#include "lite_led.h"

LiteLedSpec lite_led_for(LiteEvseState dev, bool disabled, bool online) {
  // Priority order: offline > error > policy-disabled > device sub-state.
  if (!online)                    return { {1, 1, 1}, LiteLedPattern::SlowBlink }; // white blink
  if (dev == LiteEvseState::Error) return { {1, 0, 0}, LiteLedPattern::FastBlink }; // red blink
  if (disabled)                   return { {1, 1, 0}, LiteLedPattern::Solid };     // yellow
  switch (dev) {
    case LiteEvseState::Charging:     return { {0, 1, 0}, LiteLedPattern::SlowBlink }; // green
    case LiteEvseState::Connected:    return { {0, 1, 1}, LiteLedPattern::Solid };     // cyan
    case LiteEvseState::NotConnected: return { {0, 0, 1}, LiteLedPattern::Solid };     // blue
    default:                          return { {1, 1, 1}, LiteLedPattern::Solid };     // white
  }
}

bool lite_led_phase_on(LiteLedPattern pattern, uint32_t nowMs) {
  switch (pattern) {
    case LiteLedPattern::SlowBlink: return ((nowMs / 500u) & 1u) == 0u; // 500ms on/off
    case LiteLedPattern::FastBlink: return ((nowMs / 160u) & 1u) == 0u; // ~160ms on/off
    case LiteLedPattern::Solid:
    default:                        return true;
  }
}
