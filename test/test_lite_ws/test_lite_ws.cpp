#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../src/lite/lite_ws.h"

TEST_CASE("should_push boundary") {
  CHECK_FALSE(lite_ws_should_push(0, 0, 1000));        // last==now -> false
  CHECK_FALSE(lite_ws_should_push(999, 0, 1000));      // < interval -> false
  CHECK(lite_ws_should_push(1000, 0, 1000));           // == interval -> true (inclusive)
  CHECK(lite_ws_should_push(1500, 0, 1000));           // > interval -> true
  CHECK_FALSE(lite_ws_should_push(1500, 1000, 1000));  // delta 500 -> false
}

TEST_CASE("should_push millis wrap-around") {
  // last just before the 32-bit wrap, now just after.
  uint32_t last = 0xFFFFFF00u;          // ~ -256
  uint32_t now  = 0x00000064u;          // 100  -> delta = 356
  CHECK(lite_ws_should_push(now, last, 200));        // 356 >= 200 -> true
  CHECK_FALSE(lite_ws_should_push(now, last, 500));  // 356 <  500 -> false
}
