#include "lite_override.h"

LiteOverrideAction lite_override_evaluate(const LiteOverrideLimits &limits,
                                          uint32_t sessionWh, uint32_t sessionElapsedS,
                                          bool overrideActive, bool overrideEnabling,
                                          bool autoRelease, bool chargingFalling) {
  if (!overrideActive) return LiteOverrideAction::None;

  // Limit Stop has precedence over auto-release.
  if ((limits.has_energy && sessionWh >= limits.energy_limit_wh) ||
      (limits.has_time   && sessionElapsedS >= limits.time_limit_s)) {
    return LiteOverrideAction::Stop;
  }

  // One-shot Active auto-release: release when charging falls to idle. Disabled / expired
  // overrides are sticky (overrideEnabling == false) and never auto-released here.
  if (overrideEnabling && autoRelease && chargingFalling) {
    return LiteOverrideAction::Release;
  }

  return LiteOverrideAction::None;
}
