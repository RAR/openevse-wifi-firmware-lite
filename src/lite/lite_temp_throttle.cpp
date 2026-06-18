#include "lite_temp_throttle.h"

LiteTempThrottleResult lite_temp_throttle_tick(
    const LiteTempThrottleCfg &cfg, LiteTempThrottleState &st,
    bool enabled, bool temp_valid, int temp_c, bool charging, uint32_t pilot)
{
  // Disabled: drop any active throttle and reset.
  if (!enabled) {
    if (st.start_current != 0) {
      st.start_current = 0;
      st.throttled_current = 0;
      return { LiteTempThrottleAction::Release, 0 };
    }
    return { LiteTempThrottleAction::None, 0 };
  }

  // Can't act on a temperature we don't trust.
  if (!temp_valid) {
    return { LiteTempThrottleAction::None, 0 };
  }

  if (temp_c >= cfg.setpoint_c) {
    // Engage only from an actively-charging session with a real pilot.
    if (st.start_current == 0) {
      if (!charging || pilot == 0) {
        return { LiteTempThrottleAction::None, 0 };
      }
      st.start_current = pilot;
      st.throttled_current = pilot;
    }

    if (st.throttled_current > (uint32_t)cfg.min_current_a) {
      st.throttled_current -= 1;
    }
    return { LiteTempThrottleAction::Claim, st.throttled_current };
  }

  // Below setpoint: recover if we were throttling.
  if (st.start_current != 0) {
    if (st.throttled_current < st.start_current) {
      st.throttled_current += 1;
      return { LiteTempThrottleAction::Claim, st.throttled_current };
    }
    // Fully recovered.
    st.start_current = 0;
    st.throttled_current = 0;
    return { LiteTempThrottleAction::Release, 0 };
  }

  return { LiteTempThrottleAction::None, 0 };
}
