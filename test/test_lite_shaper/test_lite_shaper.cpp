#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../src/lite/lite_shaper.h"

TEST_CASE("cap = (max_pwr - livepwr)/V + evse_present, not paused") {
  LiteShaperCfg c { 7200, 60 };           // 7200 W budget
  LiteShaperState st { 0, false };
  // live 4800 W, 240 V, evse drawing 10 A -> (7200-4800)/240 + 10 = 10 + 10 = 20 A
  CHECK(lite_shaper_cap(c, st, 4800, 240.0, 10, 0, false, 5) == doctest::Approx(20.0));
}
TEST_CASE("SOLAR divert adds self-production to the budget") {
  LiteShaperCfg c { 7200, 60 };
  LiteShaperState st { 0, false };
  // +2400 W solar -> (7200+2400-4800)/240 + 0 = 4800/240 = 20 A
  CHECK(lite_shaper_cap(c, st, 4800, 240.0, 0, 2400, true, 5) == doctest::Approx(20.0));
}
TEST_CASE("while paused, rising power is taken immediately") {
  LiteShaperCfg c { 7200, 60 };
  LiteShaperState st { 1000.0, true };     // smoothed starts at 1000
  // live 5000 > smoothed 1000 -> smoothed jumps to 5000 immediately
  lite_shaper_cap(c, st, 5000, 240.0, 0, 0, false, 5);
  CHECK(st.smoothed_live_pwr == doctest::Approx(5000.0));
}
TEST_CASE("while paused, falling power is smoothed") {
  LiteShaperCfg c { 7200, 60 };
  LiteShaperState st { 5000.0, true };
  // live 1000 < smoothed 5000 -> EMA toward 1000 (does not jump)
  lite_shaper_cap(c, st, 1000, 240.0, 0, 0, false, 5);
  CHECK(st.smoothed_live_pwr > 1000.0);
  CHECK(st.smoothed_live_pwr < 5000.0);
}
