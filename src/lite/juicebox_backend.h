#pragma once
#if defined(OPENEVSE_LITE) && defined(LITE_EVSE_BACKEND_JUICEBOX)
#include <Arduino.h>
#include "lite_evse_backend.h"
#include "juicebox_proto.h"

class JuiceBoxBackend : public LiteEvseBackend {
public:
  explicit JuiceBoxBackend(Stream &port) : _port(port) {}

  void begin() override;
  void loop()  override;

  bool          isOnline() const override;
  // Faults come from the S state code (0x05), NOT the $ES F field — F is the
  // offline-limit echo, not a fault flag (SERIAL_PROTOCOL.md §2a). The human-
  // readable cause rides the $MD/$WR channel (exposed as `wr` in /status).
  LiteEvseState getState() const override { return juicebox_map_state(_status.state); }
  int  getAmps()  const override { return _status.amps; }
  int  getPower() const override { return _status.power; }
  int  getTemp()  const override { return _status.temp; }
  int  getFault() const override { return getState() == LiteEvseState::Error ? 1 : 0; }
  // Sets the keepalive's advertised charge current. The Atmel further clamps to
  // its 6 A floor / <81 A ceiling; host-side policy lives in lite_charge_policy.
  void setChargeCurrent(int amps) override { _chargeLimit = amps; }
  // Distinct from getAmps() (the Atmel's reported max/rating in $ES field A).
  int  getChargeCurrent() const override { return _chargeLimit; }
  // Disabled => stop. The stop command IS known (RE-confirmed): commanding the active
  // limit < 6 A (we send 0 via the keepalive) clears the MCU's charge-enable gate so the
  // J1772 pilot reverts to its non-charging state and the EV stops. _enabled gates the
  // keepalive's advertised amps (see sendKeepalive). HW-validation of the stop pending a
  // complete unit (the bench unit hard-faults on GFI and won't charge).
  void setState(EvseState s) override { _enabled = (s != EvseState::Disabled); }
  bool isCharging() const override { return juicebox_map_state(_status.state) == LiteEvseState::Charging; }
  int  getMinCurrent() const override { return 6; }
  int  getMaxHardwareCurrent() const override { return _maxHwCurrent; }
  void setMaxHardwareCurrent(int a) override { _maxHwCurrent = a; }
  int  getTemperature() const override { return _status.temp; }
  bool isTemperatureValid() const override { return _status.valid; }
  int  getEvseState() const override { return _status.state; }
  void addStatusFields(JsonDocument &doc) const override;

private:
  void handleFrame(const JuiceBoxFrame &f);
  void sendKeepalive();

  Stream        &_port;
  JuiceBoxParser _parser;
  // SAFETY: zero-init => _status.valid starts false, which (with _everRx) gates
  // sendKeepalive() — no TX can occur before a real $ES is parsed. Do not add a
  // constructor to JuiceBoxStatus that leaves `valid` indeterminate.
  JuiceBoxStatus _status = {};
  unsigned long  _lastRxMillis   = 0;
  unsigned long  _lastBeatMillis = 0;
  bool           _everRx         = false;   // flips true only after a frame is received
  // RX-health diagnostics (exposed in /status): total framed inbound messages and the
  // accumulated count of RX ring-overflow events (bytes the IRQ had to drop). Climbing
  // rx_frames = receiving; flat rx_frames while the controller transmits = dropping bursts;
  // rx_overflow climbing = the receive ring can't keep up (TX/log contention on the UART).
  unsigned long  _rxFrames      = 0;
  unsigned long  _rxOverflows   = 0;
  bool           _identified    = false;   // sent ~MDNFO identify on the ~ channel once
  // Charge-current limit (A) advertised by the keepalive. Safe 6 A J1772 floor by default;
  // a future control feature will make this settable. NEVER auto-track the MCU's reported max.
  int            _chargeLimit    = 6;
  bool _enabled = true;    // Slice 1.5: Disabled => advertise the 6 A floor (no true stop cmd; see setState)
  int  _maxHwCurrent = 48; // service-max rating; seeded by the manager from config
  char _hw[24] = {0};
  char _fw[16] = {0};
  char _pv[8]  = {0};
  char _md[48] = {0};
  char _wc[24] = {0};   // last $WC payload (the handshake nonce — live-capture aid)
  char _wr[48] = {0};   // last $WR fault/report (e.g. "006:GFI Auto Test Fail")
};
#endif
