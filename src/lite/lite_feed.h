#pragma once
#include <stdint.h>

// Store for sensor values pushed in via POST /status (3c). Each value carries the
// millis() timestamp of its last update + a validity flag (false until first set).
// Setters take now_ms explicitly so the unit is pure / native-testable.
struct LiteFeed {
  int      solar_w   = 0; uint32_t solar_ms   = 0; bool solar_valid   = false;
  int      grid_ie_w = 0; uint32_t grid_ms    = 0; bool grid_valid    = false;
  double   voltage   = 0; uint32_t voltage_ms = 0; bool voltage_valid = false;
  int      shaper_w  = 0; uint32_t shaper_ms  = 0; bool shaper_valid  = false;

  // HA-integration push fields (POST /status). Vehicle SoC also feeds the /limit
  // SoC/range predicate. 0% SoC is legitimate data -> validity is tracked per field
  // (omit-when-absent), never inferred from the value.
  int      veh_soc    = 0; uint32_t veh_soc_ms   = 0; bool veh_soc_valid   = false; // battery_level (%)
  int      veh_range  = 0; uint32_t veh_range_ms = 0; bool veh_range_valid = false; // battery_range
  int      veh_eta    = 0; uint32_t veh_eta_ms   = 0; bool veh_eta_valid   = false; // time_to_full_charge (min)
  int      hbatt_soc  = 0; uint32_t hbatt_soc_ms = 0; bool hbatt_soc_valid = false; // home_battery_soc (%)
  int      hbatt_pwr  = 0; uint32_t hbatt_pwr_ms = 0; bool hbatt_pwr_valid = false; // home_battery_power (W)
};

void lite_feed_set_solar  (LiteFeed&, int w,      uint32_t now_ms);
void lite_feed_set_grid_ie(LiteFeed&, int w,      uint32_t now_ms);
void lite_feed_set_voltage(LiteFeed&, double v,   uint32_t now_ms);
void lite_feed_set_shaper (LiteFeed&, int w,      uint32_t now_ms);

void lite_feed_set_vehicle_soc  (LiteFeed&, int pct,   uint32_t now_ms);
void lite_feed_set_vehicle_range(LiteFeed&, int range, uint32_t now_ms);
void lite_feed_set_vehicle_eta  (LiteFeed&, int min,   uint32_t now_ms);
void lite_feed_set_home_battery_soc  (LiteFeed&, int pct, uint32_t now_ms);
void lite_feed_set_home_battery_power(LiteFeed&, int w,   uint32_t now_ms);

// True if `last_ms` was updated within max_age_ms of now_ms. Unsigned subtraction is
// wrap-safe across the 32-bit millis() rollover. `valid` must also be true.
bool lite_feed_fresh(bool valid, uint32_t last_ms, uint32_t now_ms, uint32_t max_age_ms);
