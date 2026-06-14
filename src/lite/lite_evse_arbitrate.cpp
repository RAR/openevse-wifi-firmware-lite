#include "lite_evse_arbitrate.h"
#include <limits.h>

EvseProperties lite_evse_arbitrate(const EvseProperties &target,
                                   const EvseClaim *claims, size_t n_claims) {
  EvseProperties result = target;
  int best_state = INT_MIN, best_cc = INT_MIN, best_mc = INT_MIN;

  for (size_t i = 0; i < n_claims; i++) {
    const EvseClaim &c = claims[i];
    if (!c.active) continue;
    // >= so a later equal-priority claim wins the tie (deterministic last-wins).
    if (c.props.getState() != EvseState::None && c.priority >= best_state) {
      result.setState(c.props.getState());
      best_state = c.priority;
    }
    if (c.props.hasChargeCurrent() && c.priority >= best_cc) {
      result.setChargeCurrent(c.props.getChargeCurrent());
      best_cc = c.priority;
    }
    if (c.props.hasMaxCurrent() && c.priority >= best_mc) {
      result.setMaxCurrent(c.props.getMaxCurrent());
      best_mc = c.priority;
    }
  }
  return result;
}
