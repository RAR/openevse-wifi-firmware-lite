#include "lite_energy_totals.h"

int32_t energy_period_day(uint32_t localEpoch)  { return (int32_t)(localEpoch / 86400u); }
// 1970-01-01 was a Thursday; +3 days shifts the floor boundary to Monday.
int32_t energy_period_week(uint32_t localEpoch) { return (int32_t)((localEpoch / 86400u + 3u) / 7u); }
int32_t energy_period_month(uint32_t localEpoch) {
  int y; unsigned m, d; lite_civil_from_secs(localEpoch, y, m, d);
  return y * 12 + (int32_t)(m - 1);
}
int32_t energy_period_year(uint32_t localEpoch) {
  int y; unsigned m, d; lite_civil_from_secs(localEpoch, y, m, d);
  return y;
}

void energy_totals_init(LiteEnergyTotals &t) {
  t.lifetime_wh = t.day_wh = t.week_wh = t.month_wh = t.year_wh = 0;
  t.day_id = t.week_id = t.month_id = t.year_id = -1;
  t.switches = 0;
}

void energy_totals_add(LiteEnergyTotals &t, uint32_t sessionWh, uint32_t localEpoch, bool clockValid) {
  t.lifetime_wh += sessionWh;
  t.switches    += 1;
  if (!clockValid) return;   // no wall clock yet -> lifetime only

  int32_t d  = energy_period_day(localEpoch);
  int32_t w  = energy_period_week(localEpoch);
  int32_t mo = energy_period_month(localEpoch);
  int32_t yr = energy_period_year(localEpoch);

  if (t.day_id   != d)  { t.day_wh   = 0; t.day_id   = d;  }
  if (t.week_id  != w)  { t.week_wh  = 0; t.week_id  = w;  }
  if (t.month_id != mo) { t.month_wh = 0; t.month_id = mo; }
  if (t.year_id  != yr) { t.year_wh  = 0; t.year_id  = yr; }

  t.day_wh   += sessionWh;
  t.week_wh  += sessionWh;
  t.month_wh += sessionWh;
  t.year_wh  += sessionWh;
}
