#include "lite_limit.h"

LiteLimitType lite_limit_type_from_string(const char *s)
{
  if (!s) return LiteLimitType::None;
  switch (s[0]) {
    case 't': return LiteLimitType::Time;
    case 'e': return LiteLimitType::Energy;
    case 's': return LiteLimitType::Soc;
    case 'r': return LiteLimitType::Range;
    case 'n':
    default:  return LiteLimitType::None;
  }
}

const char *lite_limit_type_to_string(LiteLimitType t)
{
  switch (t) {
    case LiteLimitType::Time:   return "time";
    case LiteLimitType::Energy: return "energy";
    case LiteLimitType::Soc:    return "soc";
    case LiteLimitType::Range:  return "range";
    case LiteLimitType::None:
    default:                    return "none";
  }
}

bool lite_limit_reached(const LiteLimitProps &p, uint32_t sessionElapsedS,
                        uint32_t sessionEnergyWh, int soc, int rangeKm)
{
  if (p.value == 0) return false;

  switch (p.type) {
    case LiteLimitType::Time:
      return (sessionElapsedS / 60u) >= p.value;
    case LiteLimitType::Energy:
      return sessionEnergyWh >= p.value;
    case LiteLimitType::Soc:
      // Inert until vehicle SoC is available (soc < 0).
      return soc >= 0 && (uint32_t)soc >= p.value;
    case LiteLimitType::Range:
      return rangeKm >= 0 && (uint32_t)rangeKm >= p.value;
    case LiteLimitType::None:
    default:
      return false;
  }
}
