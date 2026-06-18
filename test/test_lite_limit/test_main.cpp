#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../src/lite/lite_limit.h"

static LiteLimitProps mk(LiteLimitType t, uint32_t v) { return LiteLimitProps{ t, v, true }; }

TEST_CASE("type string round-trip") {
  CHECK(lite_limit_type_from_string("none")   == LiteLimitType::None);
  CHECK(lite_limit_type_from_string("time")   == LiteLimitType::Time);
  CHECK(lite_limit_type_from_string("energy") == LiteLimitType::Energy);
  CHECK(lite_limit_type_from_string("soc")    == LiteLimitType::Soc);
  CHECK(lite_limit_type_from_string("range")  == LiteLimitType::Range);
  CHECK(lite_limit_type_from_string(nullptr)  == LiteLimitType::None);
  CHECK(lite_limit_type_from_string("xyz")    == LiteLimitType::None);

  CHECK(std::string(lite_limit_type_to_string(LiteLimitType::Time))   == "time");
  CHECK(std::string(lite_limit_type_to_string(LiteLimitType::Energy)) == "energy");
  CHECK(std::string(lite_limit_type_to_string(LiteLimitType::Soc))    == "soc");
  CHECK(std::string(lite_limit_type_to_string(LiteLimitType::Range))  == "range");
  CHECK(std::string(lite_limit_type_to_string(LiteLimitType::None))   == "none");
}

TEST_CASE("None / zero-value never reached") {
  CHECK_FALSE(lite_limit_reached(mk(LiteLimitType::None, 0), 99999, 99999, 100, 9999));
  CHECK_FALSE(lite_limit_reached(mk(LiteLimitType::Time, 0), 99999, 99999, 100, 9999));
  CHECK_FALSE(lite_limit_reached(mk(LiteLimitType::Energy, 0), 99999, 99999, 100, 9999));
}

TEST_CASE("Time limit compares minutes, >= boundary") {
  // 30-minute limit
  CHECK_FALSE(lite_limit_reached(mk(LiteLimitType::Time, 30), 30 * 60 - 1, 0, -1, -1));
  CHECK(lite_limit_reached(mk(LiteLimitType::Time, 30), 30 * 60, 0, -1, -1));
  CHECK(lite_limit_reached(mk(LiteLimitType::Time, 30), 31 * 60, 0, -1, -1));
}

TEST_CASE("Energy limit in Wh, >= boundary") {
  CHECK_FALSE(lite_limit_reached(mk(LiteLimitType::Energy, 5000), 0, 4999, -1, -1));
  CHECK(lite_limit_reached(mk(LiteLimitType::Energy, 5000), 0, 5000, -1, -1));
}

TEST_CASE("Soc/Range inert when vehicle data unavailable (<0)") {
  CHECK_FALSE(lite_limit_reached(mk(LiteLimitType::Soc, 80), 0, 0, -1, -1));
  CHECK_FALSE(lite_limit_reached(mk(LiteLimitType::Range, 200), 0, 0, -1, -1));
}

TEST_CASE("Soc/Range honored when data present, >= boundary") {
  CHECK_FALSE(lite_limit_reached(mk(LiteLimitType::Soc, 80), 0, 0, 79, -1));
  CHECK(lite_limit_reached(mk(LiteLimitType::Soc, 80), 0, 0, 80, -1));
  CHECK_FALSE(lite_limit_reached(mk(LiteLimitType::Range, 200), 0, 0, -1, 199));
  CHECK(lite_limit_reached(mk(LiteLimitType::Range, 200), 0, 0, -1, 200));
}
