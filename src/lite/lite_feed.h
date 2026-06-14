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
};

void lite_feed_set_solar  (LiteFeed&, int w,      uint32_t now_ms);
void lite_feed_set_grid_ie(LiteFeed&, int w,      uint32_t now_ms);
void lite_feed_set_voltage(LiteFeed&, double v,   uint32_t now_ms);
void lite_feed_set_shaper (LiteFeed&, int w,      uint32_t now_ms);

// True if `last_ms` was updated within max_age_ms of now_ms. Unsigned subtraction is
// wrap-safe across the 32-bit millis() rollover. `valid` must also be true.
bool lite_feed_fresh(bool valid, uint32_t last_ms, uint32_t now_ms, uint32_t max_age_ms);
