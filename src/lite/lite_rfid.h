#pragma once
#ifdef OPENEVSE_LITE
#include "lite_evse_manager.h"
#include "lite_config_store.h"
#include "lite_rfid_policy.h"

// RFID reader integration (device glue). Owns the CLRC663 over a bit-banged SPI
// bus (pins in lite_rfid_pins.h) and gates charging through the EvseManager claim
// stack: when enabled it holds a Disabled claim (EvseClient_OpenEVSE_RFID, priority
// 1030) until a listed tag is swiped, then releases so the normal control stack
// runs for that session. On vehicle unplug it re-arms the block.
//
// Inert until lite_rfid_pins.h's LITE_RFID_PINS_CONFIRMED is defined: with pins
// unconfirmed the reader self-test can't run, so the feature FAILS OPEN (never
// blocks charging) and only reports rfid status — you can't deadlock the charger
// behind a reader we can't talk to. Live status is published in g_lite_rfid_status.
class LiteRfid {
public:
  LiteRfid() : _mgr(nullptr), _claimedDisable(false), _lastPollMs(0), _wasVehicle(false) {}

  void begin(LiteEvseManager &mgr, const LiteRfidConfig &cfg);
  void loop();
  void reconfigure(const LiteRfidConfig &cfg);

  // Enroll/scan window (UI "scan" button -> POST /rfid/add): poll the reader and publish
  // the next UID into g_lite_rfid_status.last_uid even when rfid is not enabled (no gating).
  void     startScan(uint32_t window_ms = 60000);
  uint32_t scanSecondsLeft() const;   // remaining seconds of the scan window (0 = not waiting)

private:
  void blockCharging();      // claim Disabled (gate closed)
  void releaseClaim();       // drop the RFID claim entirely
  void onAuthorized(const char *uid_hex);
  void deauthorize();        // session end -> re-arm the block

  LiteEvseManager  *_mgr;
  LiteRfidConfig    _cfg;
  LiteRfidAllowlist _allow;
  bool      _claimedDisable;
  uint32_t  _lastPollMs;
  bool      _wasVehicle;     // device-state edge: vehicle present last cycle
  uint32_t  _scanUntilMs = 0; // enroll/scan window deadline (millis); 0 = idle
};
#endif
