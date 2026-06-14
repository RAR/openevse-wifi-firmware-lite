#pragma once
#include <stdint.h>

// Session-relative override limits — a lite extension to EvseProperties (which on this
// fork has no energy/time limit fields). A limit is "set" only when its has_* flag is
// true; energy is Wh, time is seconds. Both compared against LiteSessionEnergy totals
// (session-relative, reset per plug-in) so NO wall-clock/NTP is needed.
struct LiteOverrideLimits {
  uint32_t energy_limit_wh = 0;
  uint32_t time_limit_s    = 0;
  bool     has_energy      = false;
  bool     has_time        = false;
};

enum class LiteOverrideAction : uint8_t {
  None,    // leave the override unchanged
  Stop,    // a session limit was exceeded -> caller re-asserts a sticky Disable
  Release  // an Active auto-release override hit the charge->idle edge -> caller releases
};

// Pure decision for the override-enforcement tick. See the spec for the precedence rules.
//   limits          - the active override's session limits
//   sessionWh       - LiteSessionEnergy::wattHours()
//   sessionElapsedS - LiteSessionEnergy::elapsedSecs()
//   overrideActive  - is a manual override currently claimed?
//   overrideEnabling- does the override resolve to a charging-enabling (Active) state?
//   autoRelease     - the override's auto_release flag
//   chargingFalling - true only on the tick where isCharging() went true -> false
// Precedence: a limit Stop outranks an auto-release Release.
LiteOverrideAction lite_override_evaluate(const LiteOverrideLimits &limits,
                                          uint32_t sessionWh, uint32_t sessionElapsedS,
                                          bool overrideActive, bool overrideEnabling,
                                          bool autoRelease, bool chargingFalling);
