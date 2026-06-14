#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../src/lite/lite_divert.h"

static LiteDivertCfg solar_cfg() { return { LiteDivertType::Solar, 1.1, 20, 600, 6 }; }

TEST_CASE("SOLAR available = solar / voltage; charges above trigger") {
  LiteDivertCfg c = solar_cfg();
  LiteDivertState st{0};
  // 2400 W / 240 V = 10 A. tau=20 (rising, clamps fine), big delta -> smoothed ~= 10.
  LiteDivertResult r = lite_divert_eval(c, st, 2400, 0, 240.0, 0, false, false, 100000);
  CHECK(r.available == doctest::Approx(10.0));
  CHECK(r.charge_rate_a == 10);
  CHECK(r.action == LiteDivertAction::Charge);   // 10 >= trigger(6*1.0)+0.5
}
TEST_CASE("SOLAR below trigger holds (not charging) and stops only after min-charge") {
  LiteDivertCfg c = solar_cfg();
  LiteDivertState st{0};
  // 600 W / 240 V = 2.5 A, below trigger 6.0 -> not charging yet => Hold
  LiteDivertResult r = lite_divert_eval(c, st, 600, 0, 240.0, 0, false, false, 100000);
  CHECK(r.action == LiteDivertAction::Hold);
  // Now active but min-charge NOT elapsed -> still Hold (relay/car protection)
  st.smoothed_available = 0;
  r = lite_divert_eval(c, st, 600, 0, 240.0, 0, /*active*/true, /*elapsed*/false, 100000);
  CHECK(r.action == LiteDivertAction::Hold);
  // Active AND min-charge elapsed -> Stop
  st.smoothed_available = 0;
  r = lite_divert_eval(c, st, 600, 0, 240.0, 0, true, true, 100000);
  CHECK(r.action == LiteDivertAction::Stop);
}
TEST_CASE("charge-rate rounds up only when fractional part exceeds min(1,pv_ratio)") {
  LiteDivertCfg c = solar_cfg();   // pv_ratio 1.1 -> min(1,1.1)=1.0, so never rounds up
  LiteDivertState st{0};
  LiteDivertResult r = lite_divert_eval(c, st, 2280, 0, 240.0, 0, false, false, 100000); // 9.5 A
  CHECK(r.charge_rate_a == 9);     // 9.5-9=0.5 not > 1.0
  c.pv_ratio = 0.0;                // min(1,0)=0 -> any fraction rounds up
  st.smoothed_available = 0;
  r = lite_divert_eval(c, st, 2280, 0, 240.0, 0, false, false, 100000);
  CHECK(r.charge_rate_a == 10);
}
TEST_CASE("GRID export minus EVSE draw, with reserve from pv_ratio>1") {
  LiteDivertCfg c { LiteDivertType::Grid, 1.1, 20, 600, 6 };
  LiteDivertState st{0};
  // grid_ie = -2400 W exporting / 240 = -10 A; minus evse 0 -> Igrid -10; reserve =
  // 1000*(0.1)/240 = 0.4167 A; available = 10 - 0.4167 = 9.583
  LiteDivertResult r = lite_divert_eval(c, st, 0, -2400, 240.0, 0, false, false, 100000);
  CHECK(r.available == doctest::Approx(9.5833).epsilon(0.001));
  // importing (positive grid_ie) -> no excess -> available 0 -> Hold (not active)
  st.smoothed_available = 0;
  r = lite_divert_eval(c, st, 0, 500, 240.0, 0, false, false, 100000);
  CHECK(r.available == doctest::Approx(0.0));
  CHECK(r.action == LiteDivertAction::Hold);
}
