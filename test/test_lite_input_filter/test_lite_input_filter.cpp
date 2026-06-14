#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../src/lite/lite_input_filter.h"

TEST_CASE("tau 0 disables filtering") {
  CHECK(lite_input_filter(100.0, 0.0, 0, 5) == doctest::Approx(100.0));
}
TEST_CASE("moves toward input by the decay factor") {
  // tau=10, delta=10 -> factor = 1 - e^-1 = 0.6321; from 0 toward 100 -> ~63.21
  CHECK(lite_input_filter(100.0, 0.0, 10, 10) == doctest::Approx(63.212).epsilon(0.001));
}
TEST_CASE("tau below MIN_TAU is clamped to MIN_TAU(10)") {
  // tau=2 clamps to 10, so same as the tau=10 case above
  CHECK(lite_input_filter(100.0, 0.0, 2, 10) == doctest::Approx(63.212).epsilon(0.001));
}
TEST_CASE("large delta approaches input") {
  CHECK(lite_input_filter(50.0, 0.0, 10, 1000) == doctest::Approx(50.0).epsilon(1e-6));
}
