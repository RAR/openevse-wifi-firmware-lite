#ifdef OPENEVSE_LITE
#include "lite_evse_manager.h"
#include <Arduino.h>

LiteEvseManager::LiteEvseManager(LiteEvseBackend &backend)
  : _backend(backend), _claimsVersion(0),
    _lastResolvedState(EvseState::Active), _lastCharging(false) {
  _target = EvseProperties(EvseState::Active);
  for (size_t i = 0; i < LITE_EVSE_MAX_CLAIMS; i++) { _claims[i].active = false; }
}

int LiteEvseManager::findClaim(EvseClient client) const {
  for (int i = 0; i < LITE_EVSE_MAX_CLAIMS; i++) {
    if (_claims[i].active && _claims[i].client == client) return i;
  }
  return -1;
}

bool LiteEvseManager::clientHasClaim(EvseClient client) const {
  return findClaim(client) >= 0;
}

bool LiteEvseManager::claim(EvseClient client, int priority, EvseProperties &target) {
  int idx = findClaim(client);
  if (idx < 0) {
    for (int i = 0; i < LITE_EVSE_MAX_CLAIMS; i++) {
      if (!_claims[i].active) { idx = i; break; }
    }
    if (idx < 0) return false; // registry full
  }
  _claims[idx].client = client;
  _claims[idx].priority = priority;
  _claims[idx].props = target;
  _claims[idx].active = true;
  apply();
  return true;
}

bool LiteEvseManager::release(EvseClient client) {
  int idx = findClaim(client);
  if (idx < 0) return false;
  _claims[idx].active = false;
  apply();
  return true;
}

EvseProperties &LiteEvseManager::getClaimProperties(EvseClient client) {
  if (client != EvseClient_NULL) {
    int idx = findClaim(client);
    if (idx >= 0) return _claims[idx].props;
  }
  return _target;
}

EvseState LiteEvseManager::getState(EvseClient client) {
  if (client == EvseClient_NULL) {
    return lite_evse_arbitrate(_target, _claims, LITE_EVSE_MAX_CLAIMS).getState();
  }
  int idx = findClaim(client);
  return idx >= 0 ? _claims[idx].props.getState() : EvseState::None;
}

uint32_t LiteEvseManager::getChargeCurrent(EvseClient client) {
  if (client == EvseClient_NULL) {
    return lite_evse_arbitrate(_target, _claims, LITE_EVSE_MAX_CLAIMS).getChargeCurrent();
  }
  int idx = findClaim(client);
  return idx >= 0 ? _claims[idx].props.getChargeCurrent() : UINT32_MAX;
}

uint32_t LiteEvseManager::getMaxCurrent(EvseClient client) {
  if (client == EvseClient_NULL) {
    return lite_evse_arbitrate(_target, _claims, LITE_EVSE_MAX_CLAIMS).getMaxCurrent();
  }
  int idx = findClaim(client);
  return idx >= 0 ? _claims[idx].props.getMaxCurrent() : UINT32_MAX;
}

size_t LiteEvseManager::activeClaimCount() const {
  size_t n = 0;
  for (size_t i = 0; i < LITE_EVSE_MAX_CLAIMS; i++) if (_claims[i].active) n++;
  return n;
}

void LiteEvseManager::apply() {
  EvseProperties r = lite_evse_arbitrate(_target, _claims, LITE_EVSE_MAX_CLAIMS);

  int hard = r.hasMaxCurrent() ? (int)r.getMaxCurrent() : _backend.getMaxHardwareCurrent();
  int cc   = r.hasChargeCurrent() ? (int)r.getChargeCurrent() : hard;
  if (cc > hard) cc = hard;
  cc = lite_clamp_charge_current(cc, hard);

  _backend.setState(r.getState());
  _backend.setChargeCurrent(cc);

  _claimsVersion++;

  bool charging = _backend.isCharging();
  if (r.getState() != _lastResolvedState) {
    _lastResolvedState = r.getState();
    _stateChange.fire();
  }
  if (_lastCharging && !charging) { _sessionComplete.fire(); }
  _lastCharging = charging;
}

void LiteEvseManager::loop() {
  _energy.tick(_backend.getPower(), _backend.isCharging(), millis());
}
#endif
