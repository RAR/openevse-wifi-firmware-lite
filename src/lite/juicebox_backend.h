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
  // Sets the active-limit (~AL) charge current sent to the MCU. The Atmel further clamps to
  // its 6 A floor / <81 A ceiling; host-side policy lives in lite_charge_policy. Marks the
  // command dirty so loop() pushes it immediately.
  void setChargeCurrent(int amps) override { _chargeLimit = amps; _cmdDirty = true; }
  // Distinct from getAmps() (the Atmel's reported max/rating in $ES field A).
  int  getChargeCurrent() const override { return _chargeLimit; }
  // Disabled => charge stop. The actual stop is ~AL000 (0 A pilot) in sendSetpoints(), NOT
  // ~LK: HW 2026-06-23 showed a vehicle already in J1772 State C ignores ~LK01 and keeps
  // drawing. Marks the command dirty so loop() pushes the 0 A setpoint immediately.
  void setState(EvseState s) override { _enabled = (s != EvseState::Disabled); _cmdDirty = true; }
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
  void sendFrame(const char *type, const char *payload);   // CRC-trailered ~ command/query
  void sendSetpoints();                                     // ~AL current / ~OL fallback / ~LK gate
  void txMirror(const char *s);                             // mirror exact TX bytes to /evse/console
  void consoleEmit(const char *data, size_t len);           // /evse/console emit, millis()-timestamped

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
  // /evse/console RX mirror: bytes are line-buffered here and flushed to the
  // console on LF (or when full) so the live console shows whole $/~ frames.
  char           _rxLine[96]    = {0};
  size_t         _rxLineLen     = 0;
  uint8_t        _dbgState      = 0xFF;   // last ES state mirrored to /debug/console (edge detect)
  bool           _identified    = false;   // sent ~MDCRI identify on the ~ channel once
  bool           _handshakeDone  = false;  // sent the one-shot ~PV/query/setpoint handshake
  bool           _cmdDirty       = true;   // setpoint changed -> push ~AL/~OL/~LK next loop
  unsigned long  _lastCmdMillis  = 0;      // last ~AL/~OL/~LK re-assert + ~ES poll
  // Charge-current limit (A) advertised by the keepalive. Safe 6 A J1772 floor by default;
  // a future control feature will make this settable. NEVER auto-track the MCU's reported max.
  int            _chargeLimit    = 6;
  bool _enabled = true;    // false => sendSetpoints() commands 0 A = true charge stop (HW 2026-06-23)
  int  _maxHwCurrent = 48; // service-max rating; seeded by the manager from config
  char _hw[24] = {0};
  char _fw[16] = {0};
  char _pv[8]  = {0};
  char _md[48] = {0};
  char _wc[24] = {0};   // last $WC payload (the handshake nonce — live-capture aid)
  char _wr[48] = {0};   // last $WR fault/report (e.g. "006:GFI Auto Test Fail")
};
#endif
