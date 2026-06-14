#pragma once
#ifdef OPENEVSE_LITE
#include <MicroTasks.h>
#include "lite_evse_backend.h"
#include "lite_evse_arbitrate.h"
#include "lite_charge_policy.h"
#include "lite_session_energy.h"

#define LITE_EVSE_MAX_CLAIMS 8

// EvseManager-compatible control manager over a LiteEvseBackend. Owns a base
// target (the no-claim default = Slice-1 config) + a claim registry, runs pure
// arbitration, and pushes the resolved setpoint/state to the backend.
class LiteEvseManager {
public:
  explicit LiteEvseManager(LiteEvseBackend &backend);

  bool claim(EvseClient client, int priority, EvseProperties &target);
  bool release(EvseClient client);
  bool clientHasClaim(EvseClient client) const;
  uint8_t getClaimsVersion() const { return _claimsVersion; }
  EvseProperties &getClaimProperties(EvseClient client);

  void setTargetState(EvseState s)        { _target.setState(s); apply(); }
  void setTargetChargeCurrent(uint32_t a) { _target.setChargeCurrent(a); apply(); }
  void setTargetMaxCurrent(uint32_t a)    { _target.setMaxCurrent(a); _backend.setMaxHardwareCurrent((int)a); apply(); }

  // Periodic tick: ticks the session-energy accumulator from live backend power.
  // Call once per main-loop iteration (main_lite.cpp), alongside backend.loop().
  void loop();

  EvseState getState(EvseClient client = EvseClient_NULL);
  uint32_t  getChargeCurrent(EvseClient client = EvseClient_NULL);
  uint32_t  getMaxCurrent(EvseClient client = EvseClient_NULL);

  bool isCharging() const            { return _backend.isCharging(); }
  int  getAmps() const               { return _backend.getAmps(); }
  int  getTemperature() const        { return _backend.getTemperature(); }
  bool isTemperatureValid() const    { return _backend.isTemperatureValid(); }
  int  getMinCurrent() const         { return _backend.getMinCurrent(); }
  int  getMaxHardwareCurrent() const { return _backend.getMaxHardwareCurrent(); }
  int  getEvseState() const          { return _backend.getEvseState(); }
  int           getPower() const       { return _backend.getPower(); }
  LiteEvseState getDeviceState() const { return _backend.getState(); }

  uint32_t getSessionWattSeconds() const { return _energy.wattSeconds(); }
  uint32_t getSessionWattHours()   const { return _energy.wattHours(); }
  uint32_t getSessionElapsed()     const { return _energy.elapsedSecs(); }

  void addStatusFields(JsonDocument &d) const { _backend.addStatusFields(d); }

  void onStateChange(MicroTasks::EventListener *l)     { _stateChange.Register(l); }
  void onSessionComplete(MicroTasks::EventListener *l) { _sessionComplete.Register(l); }

  size_t activeClaimCount() const;

private:
  void apply();
  int  findClaim(EvseClient client) const;

  // MicroTasks::Event::Trigger() is protected; subclass to expose a public
  // fire() so this manager can broadcast to registered listeners.
  class TriggerEvent : public MicroTasks::Event {
  public:
    void fire() { Trigger(); }
  };

  LiteEvseBackend  &_backend;
  LiteSessionEnergy _energy;
  EvseProperties    _target;
  EvseClaim        _claims[LITE_EVSE_MAX_CLAIMS];
  uint8_t          _claimsVersion;
  EvseState        _lastResolvedState;
  bool             _lastCharging;
  TriggerEvent     _stateChange;
  TriggerEvent     _sessionComplete;
};
#endif
