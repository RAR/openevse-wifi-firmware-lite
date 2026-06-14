#include "lite_divert.h"
#include "lite_input_filter.h"
#include <math.h>

LiteDivertResult lite_divert_eval(const LiteDivertCfg &cfg, LiteDivertState &st,
    int solar_w, int grid_ie_w, double voltage, int evse_present_a,
    bool currently_active, bool min_charge_elapsed, uint32_t delta_s)
{
  // Defensive: a 0/garbage voltage would make solar/voltage = inf and (int)floor(inf) UB.
  // The glue normally supplies a 240 V nominal fallback; clamp here too so the unit is safe standalone.
  if (voltage < 1.0) voltage = 240.0;

  LiteDivertResult r{};
  double available = 0.0;

  if (cfg.type == LiteDivertType::Grid) {
    double Igrid = (double)grid_ie_w / voltage - (double)evse_present_a; // grid_ie<0 = export
    if (Igrid < 0) {
      double reserve = (1000.0 * ((cfg.pv_ratio > 1.0) ? (cfg.pv_ratio - 1.0) : 0.0)) / voltage;
      available = (-Igrid - reserve);
    }
  } else { // Solar
    available = (double)solar_w / voltage;
  }
  if (available < 0) available = 0.0;

  uint32_t tau = (available > st.smoothed_available) ? cfg.attack_s : cfg.decay_s;
  st.smoothed_available = lite_input_filter(available, st.smoothed_available, tau, delta_s);

  double pvr = (cfg.pv_ratio < 1.0) ? cfg.pv_ratio : 1.0;   // min(1.0, pv_ratio)
  int rate = (int)floor(available);
  if ((available - rate) > pvr) rate += 1;
  double trigger = (double)cfg.min_current_a * pvr;

  r.available = available;
  r.smoothed = st.smoothed_available;
  r.charge_rate_a = rate;

  if (st.smoothed_available >= trigger + LITE_DIVERT_HYSTERESIS) {
    r.action = LiteDivertAction::Charge;
  } else if (st.smoothed_available <= trigger) {
    r.action = (currently_active && min_charge_elapsed) ? LiteDivertAction::Stop
                                                        : LiteDivertAction::Hold;
  } else {
    r.action = LiteDivertAction::Hold;   // inside the hysteresis band
  }
  return r;
}
