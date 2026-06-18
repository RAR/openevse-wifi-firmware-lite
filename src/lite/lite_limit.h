#pragma once
#include <stdint.h>

// Pure session-limit logic, transcribed from the standard firmware (src/limit.cpp).
// A limit stops the session once a chosen metric reaches its threshold. NOT guarded
// by OPENEVSE_LITE — compiled in the native test env.

enum class LiteLimitType : uint8_t { None = 0, Time, Energy, Soc, Range };

// First-character match, compatible with upstream LimitType::fromString
// ("none"/"time"/"energy"/"soc"/"range"). Unknown -> None.
LiteLimitType lite_limit_type_from_string(const char *s);
const char   *lite_limit_type_to_string(LiteLimitType t);

struct LiteLimitProps {
  LiteLimitType type;
  uint32_t      value;        // Time=minutes, Energy=Wh, Soc=%, Range=km
  bool          auto_release; // clear the limit automatically at session end
};

// Has the active limit been reached?  sessionElapsedS in seconds (Time compares
// minutes, matching upstream). soc/rangeKm are the vehicle metrics; pass a NEGATIVE
// value when unavailable (lite has no vehicle telemetry yet) -> Soc/Range are inert.
bool lite_limit_reached(const LiteLimitProps &p, uint32_t sessionElapsedS,
                        uint32_t sessionEnergyWh, int soc, int rangeKm);
