#pragma once
#include <stdint.h>

#ifndef LITE_DIVERT_HYSTERESIS
#define LITE_DIVERT_HYSTERESIS 0.5   // A, matches upstream EVSE_DIVERT_HYSTERESIS
#endif

enum class LiteDivertType  : uint8_t { Solar = 0, Grid = 1 };
enum class LiteDivertAction : uint8_t { Hold, Charge, Stop };  // Hold(=0, safe default) = no claim change

struct LiteDivertCfg {
  LiteDivertType type;
  double   pv_ratio;        // divert_PV_ratio (1.1)
  uint32_t attack_s;        // divert_attack_smoothing_time (20)
  uint32_t decay_s;         // divert_decay_smoothing_time (600)
  int      min_current_a;   // J1772 floor (6)
};
struct LiteDivertState { double smoothed_available; };  // carried across calls

struct LiteDivertResult {
  LiteDivertAction action;
  int    charge_rate_a;
  double available;
  double smoothed;
};

// One tick of the Eco-mode divert decision (transcribed from DivertTask::update_state).
// evse_present_a = the EVSE's own current contribution (lite passes the last commanded
// charge current — JuiceBox has no reliable live-draw readback). currently_active = does
// divert hold an Active claim now. min_charge_elapsed = has divert_min_charge_time passed
// since charging began. delta_s = seconds since the previous call (for smoothing).
LiteDivertResult lite_divert_eval(const LiteDivertCfg &cfg, LiteDivertState &st,
    int solar_w, int grid_ie_w, double voltage, int evse_present_a,
    bool currently_active, bool min_charge_elapsed, uint32_t delta_s);
