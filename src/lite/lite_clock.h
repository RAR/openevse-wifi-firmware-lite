#pragma once
#include <stdint.h>
#include <stddef.h>

// Pure software wall-clock. Seeded from an SNTP UTC epoch; free-runs on a millis()-style
// monotonic clock between syncs. No Arduino / network / flash deps -> native-testable.
class LiteClock {
public:
  static const uint32_t RESYNC_INTERVAL_MS = 3600000UL; // re-sync hourly

  void setEpoch(uint32_t utcSeconds, uint32_t nowMs) {
    _epochAtSync = utcSeconds; _msAtSync = nowMs; _valid = true;
  }
  bool valid() const { return _valid; }

  // UTC seconds = synced epoch + whole seconds elapsed since the sync. Unsigned
  // subtraction makes the millis() delta correct across a 32-bit (~49.7 d) wrap.
  uint32_t nowUtc(uint32_t nowMs) const {
    if (!_valid) return 0;
    return _epochAtSync + (uint32_t)(nowMs - _msAtSync) / 1000u;
  }
  uint32_t nowLocal(uint32_t nowMs) const {
    if (!_valid) return 0;
    return nowUtc(nowMs) + static_cast<uint32_t>(static_cast<int32_t>(_tzOffsetMin) * 60);
  }
  void setTzOffsetMinutes(int minutes) { _tzOffsetMin = minutes; }
  int  tzOffsetMinutes() const { return _tzOffsetMin; }

  bool resyncDue(uint32_t nowMs) const {
    if (!_valid) return true;
    return (uint32_t)(nowMs - _msAtSync) >= RESYNC_INTERVAL_MS;
  }

private:
  uint32_t _epochAtSync = 0;
  uint32_t _msAtSync    = 0;
  int      _tzOffsetMin = 0;
  bool     _valid       = false;
};

// Epoch seconds -> civil date (proleptic Gregorian). Howard Hinnant's algorithm.
void lite_civil_from_secs(uint32_t epoch, int &year, unsigned &month, unsigned &day);

// Epoch seconds -> "YYYY-MM-DDTHH:MM:SSZ". buf must be >= 21 bytes. Always NUL-terminates.
void lite_clock_iso8601(uint32_t epoch, char *buf, size_t cap);
