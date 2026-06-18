#ifdef OPENEVSE_LITE
#include "lite_rfid.h"
#include "lite_rfid_pins.h"
#include "lite_rfid_proto.h"
#include "lite_clrc663.h"
#include "lite_evse_state.h"
#include "lite_console.h"
#include <Arduino.h>
#include <string.h>

// ============================================================================
//  Bit-banged SPI hardware backing for the CLRC663 (only built when the pins are
//  confirmed — see lite_rfid_pins.h). Mode 0, MSB-first; CS active-low.
// ============================================================================
#ifdef LITE_RFID_PINS_CONFIRMED
#include "em_cmu.h"
#include "em_gpio.h"

static void hw_pins_init()
{
  CMU_ClockEnable(cmuClock_GPIO, true);
  GPIO_PinModeSet(LITE_RFID_SCLK_PORT, LITE_RFID_SCLK_PIN, gpioModePushPull, 0); // idle low (CPOL0)
  GPIO_PinModeSet(LITE_RFID_MOSI_PORT, LITE_RFID_MOSI_PIN, gpioModePushPull, 0);
  GPIO_PinModeSet(LITE_RFID_CS_PORT,   LITE_RFID_CS_PIN,   gpioModePushPull, 1); // CS idle high
  GPIO_PinModeSet(LITE_RFID_MISO_PORT, LITE_RFID_MISO_PIN, gpioModeInput,    0);
#ifdef LITE_RFID_HAS_PDOWN
  GPIO_PinModeSet(LITE_RFID_PDOWN_PORT, LITE_RFID_PDOWN_PIN, gpioModePushPull, 0); // PDOWN low = run
#endif
}

static uint8_t hw_xfer(void *, uint8_t out)
{
  uint8_t in = 0;
  for (int i = 7; i >= 0; --i) {
    if (out & (1 << i)) GPIO_PinOutSet(LITE_RFID_MOSI_PORT, LITE_RFID_MOSI_PIN);
    else                GPIO_PinOutClear(LITE_RFID_MOSI_PORT, LITE_RFID_MOSI_PIN);
    delayMicroseconds(LITE_RFID_SPI_HALF_US);
    GPIO_PinOutSet(LITE_RFID_SCLK_PORT, LITE_RFID_SCLK_PIN);              // rising edge: sample
    if (GPIO_PinInGet(LITE_RFID_MISO_PORT, LITE_RFID_MISO_PIN)) in |= (1 << i);
    delayMicroseconds(LITE_RFID_SPI_HALF_US);
    GPIO_PinOutClear(LITE_RFID_SCLK_PORT, LITE_RFID_SCLK_PIN);
  }
  return in;
}

static void hw_cs(void *, bool assert)
{
  if (assert) GPIO_PinOutClear(LITE_RFID_CS_PORT, LITE_RFID_CS_PIN);   // active low: select
  else        GPIO_PinOutSet(LITE_RFID_CS_PORT, LITE_RFID_CS_PIN);
}

static void hw_delay(void *, uint32_t ms) { delay(ms); }

static Clrc663Io  s_io     = { hw_xfer, hw_cs, hw_delay, nullptr };
static Clrc663    s_reader(s_io);
#endif // LITE_RFID_PINS_CONFIRMED

// ============================================================================
//  LiteRfid
// ============================================================================

void LiteRfid::begin(LiteEvseManager &mgr, const LiteRfidConfig &cfg)
{
  _mgr = &mgr;
  _cfg = cfg;
  lite_rfid_allowlist_parse(_cfg.allowlist.c_str(), &_allow);

  lite_rfid_status_reset(&g_lite_rfid_status);
  g_lite_rfid_status.enabled = _cfg.enabled;

#ifdef LITE_RFID_PINS_CONFIRMED
  hw_pins_init();
  s_reader.softReset();
  bool ok = s_reader.selfTest();
  g_lite_rfid_status.reader_ok = ok;
  if (ok) {
    s_reader.init14443aReader();
    lite_console_debugf("rfid: CLRC663 found (VERSION 0x%02X), %u allowed tag(s)",
                        s_reader.version(), (unsigned)_allow.count);
    if (_cfg.enabled) blockCharging();   // gate closed until a listed tag authorizes
  } else {
    g_lite_rfid_status.failure_count++;
    lite_console_debugf("rfid: CLRC663 self-test FAILED (VERSION 0x%02X) — failing open",
                        s_reader.version());
  }
#else
  // Pins not yet confirmed: stay inert and fail open (never block charging behind a
  // reader we can't talk to). Status reports reader_ok=false.
  g_lite_rfid_status.reader_ok = false;
  if (_cfg.enabled)
    lite_console_debugf("rfid: enabled in config but host pins not yet confirmed — inert");
#endif
}

void LiteRfid::loop()
{
  if (!_mgr) return;

  // Session-end re-arm: when the vehicle leaves (present -> NotConnected), drop any
  // authorization so the next session must swipe again.
  LiteEvseState st = _mgr->getDeviceState();
  bool vehicle = (st == LiteEvseState::Connected || st == LiteEvseState::Charging);
  if (_wasVehicle && !vehicle) deauthorize();
  _wasVehicle = vehicle;

#ifdef LITE_RFID_PINS_CONFIRMED
  if (!_cfg.enabled || !g_lite_rfid_status.reader_ok) return;

  uint32_t now = millis();
  if (now - _lastPollMs < LITE_RFID_POLL_INTERVAL_MS) return;
  _lastPollMs = now;

  Iso14443aTag tag;
  bool present = s_reader.activate(&tag);
  bool allowed = false;
  char hex[LITE_RFID_UID_HEX_MAX] = {0};
  if (present) {
    rfid_uid_to_hex(tag.uid, tag.uid_len, hex, sizeof(hex));
    allowed = lite_rfid_uid_allowed(hex, &_allow);
    strncpy(g_lite_rfid_status.last_uid, hex, sizeof(g_lite_rfid_status.last_uid) - 1);
    g_lite_rfid_status.last_uid[sizeof(g_lite_rfid_status.last_uid) - 1] = '\0';
    g_lite_rfid_status.last_allowed = allowed;
    g_lite_rfid_status.last_scan_ms = now;
  }

  LiteRfidDecideIn in{ _cfg.enabled, present, allowed, g_lite_rfid_status.authorized };
  switch (lite_rfid_decide(in)) {
    case LiteRfidAction::Authorize: onAuthorized(hex); break;
    case LiteRfidAction::Deny:
      lite_console_debugf("rfid: denied %s (not in allow-list)", hex);
      break;
    case LiteRfidAction::None: default: break;
  }
#endif // LITE_RFID_PINS_CONFIRMED
}

void LiteRfid::reconfigure(const LiteRfidConfig &cfg)
{
  _cfg = cfg;
  lite_rfid_allowlist_parse(_cfg.allowlist.c_str(), &_allow);
  g_lite_rfid_status.enabled = _cfg.enabled;

  if (!_cfg.enabled) {
    deauthorize();      // clears authorized flag (no-op on claim if none held)
    releaseClaim();     // stop gating entirely
    return;
  }
#ifdef LITE_RFID_PINS_CONFIRMED
  if (g_lite_rfid_status.reader_ok && !g_lite_rfid_status.authorized) blockCharging();
#endif
}

void LiteRfid::blockCharging()
{
  if (!_mgr) return;
  EvseProperties props(EvseState::Disabled);
  _mgr->claim(EvseClient_OpenEVSE_RFID, EvseManager_Priority_RFID, props);
  _claimedDisable = true;
  g_lite_rfid_status.authorized = false;
}

void LiteRfid::releaseClaim()
{
  if (!_mgr) return;
  if (_claimedDisable) { _mgr->release(EvseClient_OpenEVSE_RFID); _claimedDisable = false; }
}

void LiteRfid::onAuthorized(const char *uid_hex)
{
  // Step out of the way: drop the Disabled claim so the normal control stack
  // (manual/divert/schedule/base target) decides for this session.
  releaseClaim();
  g_lite_rfid_status.authorized = true;
  lite_console_debugf("rfid: authorized %s — charging enabled for this session", uid_hex);
}

void LiteRfid::deauthorize()
{
  if (!g_lite_rfid_status.authorized && !_claimedDisable) return;
  g_lite_rfid_status.authorized = false;
#ifdef LITE_RFID_PINS_CONFIRMED
  if (_cfg.enabled && g_lite_rfid_status.reader_ok) { blockCharging(); return; }
#endif
  releaseClaim();
}
#endif // OPENEVSE_LITE
