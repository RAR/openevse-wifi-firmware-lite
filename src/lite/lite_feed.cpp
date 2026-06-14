#include "lite_feed.h"

void lite_feed_set_solar  (LiteFeed &f, int w,    uint32_t now_ms) { f.solar_w = w;   f.solar_ms = now_ms;   f.solar_valid = true; }
void lite_feed_set_grid_ie(LiteFeed &f, int w,    uint32_t now_ms) { f.grid_ie_w = w; f.grid_ms = now_ms;    f.grid_valid = true; }
void lite_feed_set_voltage(LiteFeed &f, double v, uint32_t now_ms) { f.voltage = v;   f.voltage_ms = now_ms; f.voltage_valid = true; }
void lite_feed_set_shaper (LiteFeed &f, int w,    uint32_t now_ms) { f.shaper_w = w;  f.shaper_ms = now_ms;  f.shaper_valid = true; }

bool lite_feed_fresh(bool valid, uint32_t last_ms, uint32_t now_ms, uint32_t max_age_ms)
{
  return valid && ((uint32_t)(now_ms - last_ms) <= max_age_ms);
}
