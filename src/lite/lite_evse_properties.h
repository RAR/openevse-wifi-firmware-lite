#pragma once
#include <stdint.h>
#include "lite_evse_claims.h"

// Mirrors upstream EvseProperties: charge_current/max_current of UINT32_MAX mean
// "unset / no opinion" so a claim can affect only state, only current, etc.
// Intentionally NOT guarded by OPENEVSE_LITE — compiled in the native test env too.
class EvseProperties {
public:
  EvseProperties();
  explicit EvseProperties(EvseState state);

  void clear();

  EvseState getState() const          { return _state; }
  void setState(EvseState s)          { _state = s; }

  uint32_t getChargeCurrent() const   { return _charge_current; }
  void setChargeCurrent(uint32_t a)   { _charge_current = a; }
  bool hasChargeCurrent() const       { return _charge_current != UINT32_MAX; }

  uint32_t getMaxCurrent() const      { return _max_current; }
  void setMaxCurrent(uint32_t a)      { _max_current = a; }
  bool hasMaxCurrent() const          { return _max_current != UINT32_MAX; }

  bool isAutoRelease() const          { return _auto_release; }
  void setAutoRelease(bool b)         { _auto_release = b; _has_auto_release = true; }
  bool hasAutoRelease() const         { return _has_auto_release; }

  EvseProperties &operator=(const EvseProperties &rhs);

private:
  EvseState _state;
  uint32_t  _charge_current;
  uint32_t  _max_current;
  bool      _auto_release;
  bool      _has_auto_release;
};
