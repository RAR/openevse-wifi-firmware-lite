#pragma once
#include <stdint.h>

// Pure host-side session-energy accumulator. No Arduino / backend dependency —
// the caller supplies instantaneous power, a charging flag, and a monotonic
// millisecond clock. Integrates power over the interval between ticks.
//
// Session boundary: a rising edge into charging (charging goes false -> true)
// starts a fresh session, zeroing energy + elapsed. Stop (charging -> false)
// freezes the accumulated totals until the next rising edge.
// wattSeconds()/wattHours() are uint32 — ample for a single charging session
// (resets per plug-in; wraps only past ~1.19 MWh). The accumulator is `double`
// (soft-float on the M4F, which is single-precision HW-FP only) for precision
// across multi-hour sessions.
class LiteSessionEnergy {
public:
  LiteSessionEnergy() { reset(); }

  // Advance the accumulator. now_ms is a millis()-style monotonic clock;
  // unsigned subtraction handles the ~49-day wraparound correctly.
  void tick(int power_w, bool charging, uint32_t now_ms);

  uint32_t wattSeconds() const { return (uint32_t)_wattSeconds; }       // session Ws
  uint32_t wattHours()   const { return (uint32_t)(_wattSeconds / 3600.0); } // session Wh
  uint32_t elapsedSecs() const { return _elapsedSecs; }                // s since session start

  void reset();

private:
  double   _wattSeconds;   // wide accumulator: avoids overflow on long sessions
  uint32_t _elapsedSecs;
  uint32_t _sessionStartMs;
  uint32_t _lastTickMs;
  bool     _prevCharging;
  bool     _haveTick;      // false until the first tick provides a baseline timestamp
};
