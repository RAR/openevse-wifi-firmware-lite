#include "lite_shaper.h"
#include "lite_input_filter.h"

double lite_shaper_cap(const LiteShaperCfg &cfg, LiteShaperState &st,
    int live_pwr_w, double voltage, int evse_present_a,
    int solar_w, bool divert_solar_enabled, uint32_t delta_s)
{
  // Defensive: a 0/garbage voltage makes the cap inf; the glue supplies a 240 V nominal
  // fallback, clamp here too so the unit is safe standalone (matches lite_divert).
  if (voltage < 1.0) voltage = 240.0;
  int max_pwr = (int)cfg.max_pwr_w;
  int livepwr;
  if (!st.paused) {
    st.smoothed_live_pwr = live_pwr_w;
    livepwr = live_pwr_w;
  } else {
    if (live_pwr_w > st.smoothed_live_pwr) {
      st.smoothed_live_pwr = live_pwr_w;
    } else {
      st.smoothed_live_pwr = lite_input_filter(live_pwr_w, st.smoothed_live_pwr,
                                               cfg.smoothing_s, delta_s);
    }
    livepwr = (int)st.smoothed_live_pwr;
  }
  if (divert_solar_enabled) max_pwr += solar_w;     // upstream adds self-production in SOLAR mode
  return ((double)(max_pwr - livepwr) / voltage) + (double)evse_present_a;
}
