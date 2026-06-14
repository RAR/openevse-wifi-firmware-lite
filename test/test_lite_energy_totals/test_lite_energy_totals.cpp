#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../src/lite/lite_energy_totals.h"

static const uint32_t JAN01 = 1735689600u; // 2025-01-01 00:00:00 UTC
static const uint32_t JAN02 = JAN01 + 86400u;
static const uint32_t FEB01 = 1738368000u; // 2025-02-01
static const uint32_t Y2026 = 1767225600u; // 2026-01-01

TEST_CASE("init zeroes totals and marks period ids unset") {
  LiteEnergyTotals t; energy_totals_init(t);
  CHECK(t.lifetime_wh == 0u); CHECK(t.switches == 0u);
  CHECK(t.day_id == -1); CHECK(t.year_id == -1);
}

TEST_CASE("a session adds to every bucket and counts a switch") {
  LiteEnergyTotals t; energy_totals_init(t);
  energy_totals_add(t, 1500u, JAN01, true);
  CHECK(t.lifetime_wh == 1500u);
  CHECK(t.day_wh == 1500u); CHECK(t.week_wh == 1500u);
  CHECK(t.month_wh == 1500u); CHECK(t.year_wh == 1500u);
  CHECK(t.switches == 1u);
}

TEST_CASE("crossing a day boundary resets the day bucket only") {
  LiteEnergyTotals t; energy_totals_init(t);
  energy_totals_add(t, 1000u, JAN01, true);
  energy_totals_add(t, 400u,  JAN02, true);   // next day, same week/month/year
  CHECK(t.day_wh   == 400u);                  // reset, then +400
  CHECK(t.week_wh  == 1400u);                 // same week -> accumulates
  CHECK(t.month_wh == 1400u);
  CHECK(t.year_wh  == 1400u);
  CHECK(t.lifetime_wh == 1400u);
  CHECK(t.switches == 2u);
}

TEST_CASE("crossing a week boundary (same month) resets day+week, not month/year") {
  LiteEnergyTotals t; energy_totals_init(t);
  energy_totals_add(t, 1000u, 1735689600u, true);              // 2025-01-01 (Wed)
  energy_totals_add(t, 300u,  1735689600u + 7u * 86400u, true); // 2025-01-08 (Wed) next week, same month
  CHECK(t.day_wh   == 300u);   // new day -> reset
  CHECK(t.week_wh  == 300u);   // new week -> reset
  CHECK(t.month_wh == 1300u);  // same month -> accumulates
  CHECK(t.year_wh  == 1300u);  // same year -> accumulates
}

TEST_CASE("crossing month and year boundaries resets those buckets") {
  LiteEnergyTotals t; energy_totals_init(t);
  energy_totals_add(t, 1000u, JAN01, true);
  energy_totals_add(t, 700u,  FEB01, true);   // new month, same year
  CHECK(t.month_wh == 700u);
  CHECK(t.year_wh  == 1700u);
  energy_totals_add(t, 200u,  Y2026, true);   // new year
  CHECK(t.year_wh  == 200u);
  CHECK(t.lifetime_wh == 1900u);
}

TEST_CASE("pre-sync sessions add to lifetime only (no calendar bucket)") {
  LiteEnergyTotals t; energy_totals_init(t);
  energy_totals_add(t, 800u, 0u, false);      // clock not valid
  CHECK(t.lifetime_wh == 800u);
  CHECK(t.switches == 1u);
  CHECK(t.day_wh == 0u); CHECK(t.day_id == -1);  // untouched
}

TEST_CASE("period ids: day, Monday-week, month, year") {
  CHECK(energy_period_day(JAN01)   == (int32_t)(JAN01 / 86400u));
  CHECK(energy_period_month(JAN01) == 2025 * 12 + 0);
  CHECK(energy_period_month(FEB01) == 2025 * 12 + 1);
  CHECK(energy_period_year(JAN01)  == 2025);
  CHECK(energy_period_year(Y2026)  == 2026);
  // 2025-01-01 is a Wednesday; Wed and Fri are the same Monday-aligned week, next Monday differs.
  CHECK(energy_period_week(JAN01) == energy_period_week(JAN01 + 2u * 86400u));
  CHECK(energy_period_week(JAN01) != energy_period_week(JAN01 + 5u * 86400u));
}
