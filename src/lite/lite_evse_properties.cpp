#include "lite_evse_properties.h"

EvseProperties::EvseProperties() { clear(); }

EvseProperties::EvseProperties(EvseState state) {
  clear();
  _state = state;
}

void EvseProperties::clear() {
  _state            = EvseState::None;
  _charge_current   = UINT32_MAX;
  _max_current      = UINT32_MAX;
  _auto_release     = false;
  _has_auto_release = false;
}

EvseProperties &EvseProperties::operator=(const EvseProperties &rhs) {
  _state            = rhs._state;
  _charge_current   = rhs._charge_current;
  _max_current      = rhs._max_current;
  _auto_release     = rhs._auto_release;
  _has_auto_release = rhs._has_auto_release;
  return *this;
}
