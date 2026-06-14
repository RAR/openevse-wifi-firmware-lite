#pragma once
#include <stdint.h>

struct LiteShaperCfg { uint32_t max_pwr_w; uint32_t smoothing_s; }; // smp, sst
struct LiteShaperState { double smoothed_live_pwr; bool paused; };

// Max-current cap (A) the shaper allows (transcribes CurrentShaperTask::shapeCurrent).
// When not paused, live power is used raw; while paused, rising power is taken immediately,
// falling power is smoothed. When divert is enabled in SOLAR mode, self-production adds to
// the budget (max_pwr += solar). Single-phase. evse_present_a = EVSE's own current.
// Caller treats a cap below the min charge current as "pause".
double lite_shaper_cap(const LiteShaperCfg &cfg, LiteShaperState &st,
    int live_pwr_w, double voltage, int evse_present_a,
    int solar_w, bool divert_solar_enabled, uint32_t delta_s);
