#pragma once
#include <stdint.h>

// Pure temperature-throttle decision, transcribed from the standard firmware's
// TempThrottleTask::loop (src/temp_throttle.cpp). One call = one throttle tick.
// Intentionally NOT guarded by OPENEVSE_LITE — compiled in the native test env.
//
// Behaviour: once the unit is charging and temperature reaches the setpoint, the
// commanded charge current ramps DOWN 1 A per tick toward the J1772 floor; once
// temperature drops back below the setpoint it ramps UP 1 A per tick toward the
// current captured when throttling first engaged, then releases entirely.

struct LiteTempThrottleCfg {
  int setpoint_c;     // engage threshold (deg C)
  int min_current_a;  // J1772 floor the throttle will not go below
};

// Carried across ticks. start_current == 0 means "not currently throttling".
struct LiteTempThrottleState {
  uint32_t start_current;     // pilot captured when throttling engaged (0 = idle)
  uint32_t throttled_current; // current the throttle is presently commanding
};

enum class LiteTempThrottleAction : uint8_t {
  None,     // leave any existing claim untouched
  Claim,    // (re)assert the throttle claim at charge_current
  Release   // drop the throttle claim
};

struct LiteTempThrottleResult {
  LiteTempThrottleAction action;
  uint32_t charge_current;    // valid only when action == Claim
};

// enabled/temp_valid/temp_c/charging/pilot come from the live manager. pilot is the
// charge current the throttle should clamp down from (getChargeCurrent()).
LiteTempThrottleResult lite_temp_throttle_tick(
    const LiteTempThrottleCfg &cfg, LiteTempThrottleState &st,
    bool enabled, bool temp_valid, int temp_c, bool charging, uint32_t pilot);
