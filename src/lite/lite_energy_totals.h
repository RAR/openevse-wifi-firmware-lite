#pragma once
#include <stdint.h>
#include "lite_clock.h"   // lite_civil_from_secs for month/year bucketing

// Persistent lifetime energy totals, bucketed by local calendar period. POD: persisted
// verbatim as one FlashDB blob. Wh internally (uint64 -> no overflow); kWh emitted at /status.
struct LiteEnergyTotals {
  uint64_t lifetime_wh;
  uint64_t day_wh, week_wh, month_wh, year_wh;
  int32_t  day_id, week_id, month_id, year_id;  // local-calendar period ids; -1 = unset
  uint32_t switches;                            // completed-session count
};
// Persisted verbatim to flash — pin the layout so a field change can't silently
// corrupt stored totals (bump the expected size deliberately if you change fields).
static_assert(sizeof(LiteEnergyTotals) == 64, "LiteEnergyTotals layout changed — migration needed");

void energy_totals_init(LiteEnergyTotals &t);   // zero + ids = -1

// Add a completed session's Wh at local-time `localEpoch`. Resets any bucket whose period
// id changed since the last add, then adds. If !clockValid, adds to lifetime + switches only
// (no calendar bucketing). Always increments switches.
void energy_totals_add(LiteEnergyTotals &t, uint32_t sessionWh, uint32_t localEpoch, bool clockValid);

// Local-calendar period ids (pure). day/week need no civil math; week is Monday-aligned.
int32_t energy_period_day(uint32_t localEpoch);
int32_t energy_period_week(uint32_t localEpoch);
int32_t energy_period_month(uint32_t localEpoch);
int32_t energy_period_year(uint32_t localEpoch);
