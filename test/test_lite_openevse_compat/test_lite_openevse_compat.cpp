#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include <string.h>
#include "../../src/lite/lite_openevse_compat.h"

TEST_CASE("maps canonical states to OpenEVSE state ints") {
  CHECK(openevse_state_code(LiteEvseState::Unknown,      false) == 0);
  CHECK(openevse_state_code(LiteEvseState::NotConnected, false) == 1);
  CHECK(openevse_state_code(LiteEvseState::Connected,    false) == 2);
  CHECK(openevse_state_code(LiteEvseState::Charging,     false) == 3);
  CHECK(openevse_state_code(LiteEvseState::Error,        false) == 8);
}

TEST_CASE("control-disabled overrides physical state with sleeping (254)") {
  CHECK(openevse_state_code(LiteEvseState::Charging,     true) == 254);
  CHECK(openevse_state_code(LiteEvseState::NotConnected, true) == 254);
  CHECK(openevse_state_code(LiteEvseState::Error,        true) == 8); // fault still wins
}

TEST_CASE("status strings match the state mapping") {
  CHECK(strcmp(openevse_status_str(LiteEvseState::Unknown,      false), "unknown") == 0);
  CHECK(strcmp(openevse_status_str(LiteEvseState::NotConnected, false), "not connected") == 0);
  CHECK(strcmp(openevse_status_str(LiteEvseState::Connected,    false), "connected") == 0);
  CHECK(strcmp(openevse_status_str(LiteEvseState::Charging,     false), "charging") == 0);
  CHECK(strcmp(openevse_status_str(LiteEvseState::Error,        false), "error") == 0);
  CHECK(strcmp(openevse_status_str(LiteEvseState::Charging,     true),  "sleeping") == 0);
}
