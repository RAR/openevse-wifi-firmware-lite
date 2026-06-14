#pragma once
#include <stdint.h>
#include "lite_evse_state.h"

struct LiteLedColor { bool r, g, b; };
enum class LiteLedPattern : uint8_t { Solid, SlowBlink, FastBlink };
struct LiteLedSpec { LiteLedColor color; LiteLedPattern pattern; };

// Resolve the indicator for the current condition. Priority: offline > error > disabled >
// device sub-state. `disabled` = policy state (manager target Disabled); `online` = backend
// comms liveness. Pure.
LiteLedSpec lite_led_for(LiteEvseState dev, bool disabled, bool online);

// Is the LED in its lit half at nowMs? Solid -> always true. Pure.
bool lite_led_phase_on(LiteLedPattern pattern, uint32_t nowMs);
