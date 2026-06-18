#include "lite_feed.h"

void lite_feed_set_solar  (LiteFeed &f, int w,    uint32_t now_ms) { f.solar_w = w;   f.solar_ms = now_ms;   f.solar_valid = true; }
void lite_feed_set_grid_ie(LiteFeed &f, int w,    uint32_t now_ms) { f.grid_ie_w = w; f.grid_ms = now_ms;    f.grid_valid = true; }
void lite_feed_set_voltage(LiteFeed &f, double v, uint32_t now_ms) { f.voltage = v;   f.voltage_ms = now_ms; f.voltage_valid = true; }
void lite_feed_set_shaper (LiteFeed &f, int w,    uint32_t now_ms) { f.shaper_w = w;  f.shaper_ms = now_ms;  f.shaper_valid = true; }

void lite_feed_set_vehicle_soc  (LiteFeed &f, int pct,   uint32_t now_ms) { f.veh_soc = pct;     f.veh_soc_ms = now_ms;   f.veh_soc_valid = true; }
void lite_feed_set_vehicle_range(LiteFeed &f, int range, uint32_t now_ms) { f.veh_range = range; f.veh_range_ms = now_ms; f.veh_range_valid = true; }
void lite_feed_set_vehicle_eta  (LiteFeed &f, int min,   uint32_t now_ms) { f.veh_eta = min;     f.veh_eta_ms = now_ms;   f.veh_eta_valid = true; }
void lite_feed_set_home_battery_soc  (LiteFeed &f, int pct, uint32_t now_ms) { f.hbatt_soc = pct; f.hbatt_soc_ms = now_ms; f.hbatt_soc_valid = true; }
void lite_feed_set_home_battery_power(LiteFeed &f, int w,   uint32_t now_ms) { f.hbatt_pwr = w;   f.hbatt_pwr_ms = now_ms; f.hbatt_pwr_valid = true; }

bool lite_feed_fresh(bool valid, uint32_t last_ms, uint32_t now_ms, uint32_t max_age_ms)
{
  return valid && ((uint32_t)(now_ms - last_ms) <= max_age_ms);
}
