#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../src/lite/lite_evse_arbitrate.h"

static EvseProperties target() {
  EvseProperties t(EvseState::Active);
  t.setChargeCurrent(32);
  t.setMaxCurrent(40);
  return t;
}

TEST_CASE("no active claims => resolved equals target") {
  EvseClaim claims[4] = {};
  EvseProperties r = lite_evse_arbitrate(target(), claims, 4);
  CHECK(r.getState() == EvseState::Active);
  CHECK(r.getChargeCurrent() == 32u);
  CHECK(r.getMaxCurrent() == 40u);
}

TEST_CASE("a claim that sets only charge_current overrides current, not state") {
  EvseClaim claims[2] = {};
  claims[0].client = EvseClient_OpenEVSE_Divert;
  claims[0].priority = EvseManager_Priority_Divert;
  claims[0].props.setChargeCurrent(12);   // state stays None => no opinion
  claims[0].active = true;
  EvseProperties r = lite_evse_arbitrate(target(), claims, 2);
  CHECK(r.getChargeCurrent() == 12u);
  CHECK(r.getState() == EvseState::Active); // unchanged
}

TEST_CASE("higher priority wins a field; lower-priority Disabled loses to higher Active") {
  EvseClaim claims[2] = {};
  claims[0].client = EvseClient_OpenEVSE_Limit;
  claims[0].priority = EvseManager_Priority_Limit;     // 1100
  claims[0].props.setState(EvseState::Disabled);
  claims[0].active = true;
  claims[1].client = EvseClient_OpenEVSE_TempThrottle;
  claims[1].priority = EvseManager_Priority_Safety;    // 5000 (higher)
  claims[1].props.setState(EvseState::Active);
  claims[1].active = true;
  EvseProperties r = lite_evse_arbitrate(target(), claims, 2);
  CHECK(r.getState() == EvseState::Active); // safety(5000) beats limit(1100)
}

TEST_CASE("Disabled at higher priority wins") {
  EvseClaim claims[1] = {};
  claims[0].client = EvseClient_OpenEVSE_Limit;
  claims[0].priority = EvseManager_Priority_Limit;
  claims[0].props.setState(EvseState::Disabled);
  claims[0].active = true;
  EvseProperties r = lite_evse_arbitrate(target(), claims, 1);
  CHECK(r.getState() == EvseState::Disabled);
}

TEST_CASE("inactive and None-state/unset claims are ignored") {
  EvseClaim claims[2] = {};
  claims[0].client = EvseClient_OpenEVSE_Manual;
  claims[0].priority = EvseManager_Priority_Manual;
  claims[0].props.setChargeCurrent(6);
  claims[0].active = false;               // inactive -> ignored
  claims[1].active = true;                // active but sets nothing (state None, currents unset)
  EvseProperties r = lite_evse_arbitrate(target(), claims, 2);
  CHECK(r.getChargeCurrent() == 32u);     // target preserved
  CHECK(r.getState() == EvseState::Active);
}

TEST_CASE("tie on priority: later claim in array wins") {
  EvseClaim claims[2] = {};
  claims[0].client = EvseClient_OpenEVSE_MQTT;
  claims[0].priority = EvseManager_Priority_API;  // 500
  claims[0].props.setChargeCurrent(10);
  claims[0].active = true;
  claims[1].client = EvseClient_OpenEVSE_Ohm;
  claims[1].priority = EvseManager_Priority_Ohm;  // 500 (tie)
  claims[1].props.setChargeCurrent(20);
  claims[1].active = true;
  EvseProperties r = lite_evse_arbitrate(target(), claims, 2);
  CHECK(r.getChargeCurrent() == 20u);     // last wins on tie
}
