#pragma once
#include <stddef.h>
#include "lite_evse_properties.h"

struct EvseClaim {
  EvseClient     client;
  int            priority;
  EvseProperties props;
  bool           active;
};

// Per-field highest-priority-wins, starting from `target`. Among active claims,
// a claim overrides a field only if it "sets" it: state != None, or
// hasChargeCurrent(), or hasMaxCurrent(). Highest priority wins; on a tie the
// later claim in the array wins. The resolved state is always set (target.state
// if no active claim sets state). Pure — native-tested.
EvseProperties lite_evse_arbitrate(const EvseProperties &target,
                                   const EvseClaim *claims, size_t n_claims);
