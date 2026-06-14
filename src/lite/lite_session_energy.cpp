#include "lite_session_energy.h"

void LiteSessionEnergy::reset() {
  _wattSeconds    = 0.0;
  _elapsedSecs    = 0;
  _sessionStartMs = 0;
  _lastTickMs     = 0;
  _prevCharging   = false;
  _haveTick       = false;
}

void LiteSessionEnergy::tick(int power_w, bool charging, uint32_t now_ms) {
  // Rising edge into charging -> start a fresh session.
  if (charging && !_prevCharging) {
    _wattSeconds    = 0.0;
    _elapsedSecs    = 0;
    _sessionStartMs = now_ms;
  }

  // Integrate only while charging and once we have a prior timestamp.
  if (charging && _haveTick && _prevCharging) {   // skip the rising-edge tick: no real interval to attribute, and avoids folding the stopped gap into the new session
    uint32_t dt_ms = now_ms - _lastTickMs;   // unsigned: wraps correctly
    // Negative power (V2G / reverse metering) is intentionally ignored.
    if (power_w > 0) {
      _wattSeconds += (double)power_w * (double)dt_ms / 1000.0;
    }
    _elapsedSecs = (now_ms - _sessionStartMs) / 1000;
  }

  _lastTickMs   = now_ms;
  _haveTick     = true;
  _prevCharging = charging;
}
