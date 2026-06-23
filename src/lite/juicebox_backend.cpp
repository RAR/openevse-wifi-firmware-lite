#if defined(OPENEVSE_LITE) && defined(LITE_EVSE_BACKEND_JUICEBOX)
#include "juicebox_backend.h"
#include <string.h>
#include "lt_family.h"   // USART0 register access (em_device) — RX-wedge diagnostics
#include "lite_console.h"   // /evse/console live mirror of the $/~ wire

// Read-and-clear the USART0 RX ring-overflow flag (silabs core; declared in the core's
// SerialPrivate.h, which app code doesn't include). True => the RX IRQ had to drop bytes
// because the ring was full — i.e. we couldn't drain fast enough (TX/log contention).
extern bool SerialClass_getOverflow();

// Comm-watchdog reload is 3000 ticks; tick period unproven (RE flagged ~3 s LIKELY, up to
// ~30 s). 15 s keepalive doubles as the empirical probe: if the MCU stays online at this
// spacing the watchdog is >=15 s; if it flaps offline between beats, it's shorter (tighten
// back down). Reduce once the timeout is HW-confirmed.
static const unsigned long JB_KEEPALIVE_INTERVAL_MS = 15000;
// Offline timeout must EXCEED the Atmel's slowest liveness frame. HW-observed
// 2026-06-13: in steady state the ONLY inbound frame is a $TP ping every ~63 s
// (metronomic; $ES/$MD only at boot/on-change). A 5 s timeout made isOnline()
// flap (true for 5 s after each ping, false for the other ~58 s). 90 s clears one
// ping interval with margin without flapping; a real dead controller still trips
// offline within ~90 s. Bump toward ~130 s if we ever need to ride out a missed ping.
static const unsigned long JB_OFFLINE_TIMEOUT_MS    = 90000;
// How often to re-assert the active setpoint (~AL/~OL/~LK) + poll status (~ES). The OEM
// re-asserts ~AL ~every 63 s; 15 s keeps our commanded limit fresh and status current
// without flooding the shared UART. Setpoint CHANGES are sent immediately (see _cmdDirty).
static const unsigned long JB_CMD_INTERVAL_MS       = 15000;

void JuiceBoxBackend::begin() {
  _lastBeatMillis = millis();
}

void JuiceBoxBackend::loop() {
  JuiceBoxFrame f;
  while (_port.available() > 0) {
    int b = _port.read();
    if (b < 0) break;
    // Mirror the raw RX byte to /evse/console, line-buffered: append until LF (or
    // the buffer fills), then flush one whole frame. Always buffer (cheap) so line
    // framing is correct regardless of when a console connects; only the flush is gated.
    if (_rxLineLen < sizeof(_rxLine) - 1) _rxLine[_rxLineLen++] = (char)b;
    if (b == '\n' || _rxLineLen >= sizeof(_rxLine) - 1) {
      if (lite_console_has_client(LITE_CONSOLE_EVSE))
        lite_console_broadcast(LITE_CONSOLE_EVSE, (const uint8_t *)_rxLine, _rxLineLen);
      _rxLineLen = 0;
    }
    if (_parser.feed((uint8_t)b, f)) {
      _lastRxMillis = millis();
      _everRx = true;
      _rxFrames++;
      handleFrame(f);
    }
  }
  // Accumulate RX ring-overflow events (read-and-clear). Polled once per loop; if the
  // ring filled since last loop, we dropped inbound bytes — a direct measure of whether
  // our own TX/logging on the shared UART is starving reception.
  if (SerialClass_getOverflow()) _rxOverflows++;

  // Identify on the '~' channel once we know the controller is there, mimicking the stock
  // Gecko OS boot's ~MDCRI: identity dump (values from the 2026-05-26 OEM capture). The
  // ATmega sniffs the '~' channel for the WiFi module's presence; the OEM streamed exactly
  // this. Sent once after first contact. (Identity strings are this bench unit's; for
  // production each unit differs. UNVALIDATED for charging — GFI HW fault gates that anyway.)
  if (_everRx && !_identified) {
    // LF (\n) only — byte-faithful to the OEM (its console staircases = LF without CR).
    static const char *kIdentity[] = {
      "~MDCRI:EMWERK-JB201-1.0.46, Gecko_OS-STANDARD-4.2.7-11064, WGM160P\n",
      "~MDCRI:Zn UUID EADE2FF30BE60E508EF4C515F4B3B1FFFEA21297\n",
      "~MDCRI:JNetID:<0910040100000500000000620001>\n",
      "~MDCRI:EMM SERIAL:<  17EMOTORWERKS00030>\n",
    };
    for (const char *line : kIdentity) { _port.print(line); txMirror(line); }
    _identified = true;
#ifdef JB_DEBUG
    LT_I("JBTX(~): #MDCRI identity burst (FW/UUID/JNetID/serial)");
#endif
  }

  unsigned long now = millis();

  // After identity, run the stock-style handshake ONCE: announce protocol version, query
  // identity/status (empty-payload ~FW/~HW/~CR/~ES -> the MCU replies $FW/$HW/$CR/$ES, which
  // handleFrame() parses), then push the initial setpoints. Mirrors the OEM WiFi module
  // (captured 2026-06-17); every frame carries the required CRC trailer (juicebox_build_*).
  if (_identified && !_handshakeDone) {
    sendFrame("PV", "20");
    sendFrame("FW", nullptr);
    sendFrame("HW", nullptr);
    sendFrame("CR", nullptr);
    sendFrame("ES", nullptr);
    sendSetpoints();
    _handshakeDone = true;
    _cmdDirty      = false;
    _lastCmdMillis = now;
  }

  // Keepalive holds the comm watchdog via a '~'-channel heartbeat (see sendKeepalive) —
  // matching how the stock module keeps the MCU online. Gated on _everRx so we don't
  // transmit into a silent line.
  if (_everRx && (now - _lastBeatMillis) >= JB_KEEPALIVE_INTERVAL_MS) {
    sendKeepalive();
    _lastBeatMillis = now;
  }

  // Drive the MCU: re-assert the active setpoint (~AL current / ~OL fallback / ~LK gate) and
  // poll status (~ES). Sent immediately on a setpoint change (_cmdDirty) and periodically
  // re-asserted. This is what actually sets charge current / start-stop on the controller.
  if (_handshakeDone && (_cmdDirty || (now - _lastCmdMillis) >= JB_CMD_INTERVAL_MS)) {
    sendSetpoints();
    sendFrame("ES", nullptr);
    _cmdDirty      = false;
    _lastCmdMillis = now;
  }
}

bool JuiceBoxBackend::isOnline() const {
  return _everRx && (millis() - _lastRxMillis) < JB_OFFLINE_TIMEOUT_MS;
}

// Copy a frame payload into a fixed display buffer, bounded by the frame's declared
// length so the trailing :tag: (added by a foreign layer, beyond the $-protocol's
// length-delimited payload) is stripped. Never reads past the parsed (NUL-terminated)
// payload. Always NUL-terminates.
static void copy_bounded(char *dst, size_t cap, const char *src, uint16_t len) {
  size_t n = strlen(src);
  if (n > len)      n = len;       // strip anything past the declared length (the :tag:)
  if (n > cap - 1)  n = cap - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

#ifdef JB_DEBUG
// Copy `src` into `dst`, replacing every '$' with '#'. The LT debug log shares the
// physical UART with the JuiceBox protocol line, and the Atmel resyncs its parser on
// '$' — so a literal '$' in a log string would look like a frame-start and corrupt
// framing. Obscure it. Always NUL-terminates.
static void jb_log_safe(char *dst, size_t cap, const char *src) {
  size_t i = 0;
  for (; src[i] && i + 1 < cap; ++i) dst[i] = (src[i] == '$') ? '#' : src[i];
  dst[i] = '\0';
}
#endif

void JuiceBoxBackend::handleFrame(const JuiceBoxFrame &f) {
#ifdef JB_DEBUG
  // Bench-debug echo of every received Atmel frame (raw payload incl. any foreign
  // :tag:), with '$' obscured (see jb_log_safe). Built only in [env:openevse_lite_debug];
  // compiled out of the production env.
  {
    char safe[JB_MAX_PAYLOAD + 1];
    jb_log_safe(safe, sizeof(safe), f.payload);
    LT_I("JBRX [%s] len=%u: %s", f.type, (unsigned)f.len, safe);
  }
#endif
  // '~' = WiFi-module identity/version channel (decoded from the ATmega flash). The MCU
  // sends "~JV:?" to query the host's version and advances its handshake on a "~JV:!<n>$"
  // reply. We answer with version 1. (Exact <n> + ordering UNVALIDATED vs a live OEM
  // capture; the parser accepts any small int.) ~ frames carry no EVSE status — return.
  if (f.start == '~') {
    if (!strcmp(f.type, "JV") && f.payload[0] == '?') {
      _port.print("~JV:!1$\n");   // LF only, matching the OEM line terminator
      txMirror("~JV:!1$\n");
#ifdef JB_DEBUG
      LT_I("JBTX(~): #JV:!1#  (reply to ~JV:? version query)");
#endif
    }
    return;
  }
  if      (!strcmp(f.type, "ES")) {
    juicebox_parse_es(f.payload, f.len, _status);
    if (_status.state != _dbgState) {   // device state edge -> /debug/console firmware event
      lite_console_debugf("evse state 0x%02X -> 0x%02X (%s) A=%d pilotV=%d",
                          _dbgState, _status.state,
                          lite_evse_state_name(juicebox_map_state(_status.state)),
                          _status.amps, _status.h);
      _dbgState = _status.state;
    }
#ifdef JB_DEBUG
    // Decoded view so we can SEE the live mapping. The wire letters are misleading
    // (SERIAL_PROTOCOL.md §2a) — labels below show the REAL meaning. S is hex.
    LT_I("  ES: S=0x%02X->%s | A(amps)=%d H(pilotV)=%d P(pilotduty)=%d F(offlim)=%d L=%d T(TPecho)=%d",
         _status.state, lite_evse_state_name(juicebox_map_state(_status.state)),
         _status.amps, _status.h, _status.power, _status.offline_limit, _status.line, _status.temp);
#endif
  }
  else if (!strcmp(f.type, "HW")) { copy_bounded(_hw, sizeof(_hw), f.payload, f.len); }
  else if (!strcmp(f.type, "FW")) { copy_bounded(_fw, sizeof(_fw), f.payload, f.len); }
  else if (!strcmp(f.type, "PV")) { copy_bounded(_pv, sizeof(_pv), f.payload, f.len); }
  else if (!strcmp(f.type, "MD")) { copy_bounded(_md, sizeof(_md), f.payload, f.len);
                                    lite_console_debugf("evse note: %s", _md); }
  else if (!strcmp(f.type, "WC")) { copy_bounded(_wc, sizeof(_wc), f.payload, f.len); }
  else if (!strcmp(f.type, "WR")) { copy_bounded(_wr, sizeof(_wr), f.payload, f.len);
                                    lite_console_debugf("evse report: %s", _wr); }
  // other types ignored this slice
}

void JuiceBoxBackend::sendKeepalive() {
  // Heartbeat on the '~' channel, mimicking the stock Gecko OS module's idle telemetry
  // (~MDNFO:V1:65535/A1:65535 — meter sentinel, 65535 = no meter). Any assembled line
  // clears the ATmega's offline bit (0xa14, common path) so this holds the comm watchdog
  // online exactly like the OEM did, and it carries the "~MDNFO" substring the MCU sniffs
  // for WiFi-module presence (strstr @0x6790). This REPLACES the old keepalive idea: the
  // OEM keeps the MCU alive purely via '~' chatter (its console shows no '$' frames).
  // The ~AL amps-set (juicebox_build_amps_set) is the separate charge-current command, and
  // ~LK (juicebox_build_lock) the start/stop gate — both now carry the required CRC trailer
  // and are sent on demand when we drive current, not as the heartbeat. (Charge control still
  // UNVALIDATED on HW: the GFI/No-GND self-test gates the contactor on this bench regardless.)
  _port.print("~MDNFO:V1:65535/A1:65535\n");   // LF only, matching the OEM line terminator
  txMirror("~MDNFO:V1:65535/A1:65535\n");
#ifdef JB_DEBUG
  LT_I("JBTX(~): #MDNFO:V1:65535/A1:65535  (~ keepalive)");
#endif
}

// Mirror exactly `s` (whatever was written to the UART, terminator included) to the
// /evse/console live stream. Byte-faithful; a no-op (one cheap check) when unwatched.
void JuiceBoxBackend::txMirror(const char *s) {
  if (s && *s && lite_console_has_client(LITE_CONSOLE_EVSE))
    lite_console_broadcast(LITE_CONSOLE_EVSE, (const uint8_t *)s, strlen(s));
}

// Build a CRC-trailered WiFi->MCU command frame and write it to the UART (LF-terminated,
// matching the OEM). payload == nullptr / "" makes the empty-payload query form (~XX000:).
void JuiceBoxBackend::sendFrame(const char *type, const char *payload) {
  char buf[48];
  if (juicebox_build_frame(type, payload, buf, sizeof(buf))) {
    size_t n = strlen(buf);
    if (n + 1 < sizeof(buf)) { buf[n] = '\n'; buf[n + 1] = '\0'; }  // fold LF in so TX is one mirror
    _port.print(buf);
    txMirror(buf);
#ifdef JB_DEBUG
    char safe[48]; jb_log_safe(safe, sizeof(safe), buf);   // obscure '$' (none here, but safe)
    LT_I("JBTX(~): %s", safe);
#endif
  }
}

// Push the active setpoint to the MCU: ~AL (active current limit = the J1772 charge gate),
// ~OL (offline/fallback limit, mirrored), ~LK (connector lock). Driven by the EVSE manager
// via setChargeCurrent()/setState(); amps clamped to [0,79] in the builder.
//
// CHARGE STOP = ~AL000 (collapse the advertised pilot current), NOT ~LK. HW 2026-06-23:
// a vehicle already in J1772 State C ignores ~LK01 and keeps drawing, so on Disabled
// (manual Off / a tripped limit) we command 0 A and the pilot drops -> the car stops. ~OL
// is mirrored to 0 too so a comms-loss while disabled also stays stopped (fail-safe). ~LK
// is still asserted (it's the physical connector lock, harmless either way).
void JuiceBoxBackend::sendSetpoints() {
  int amps = (!_enabled || _chargeLimit < 0) ? 0 : _chargeLimit;
  char buf[48];
  // Each builder writes a frame; fold in the LF, send, and mirror to /evse/console.
  auto emit = [this, &buf](bool built) {
    if (!built) return;
    size_t n = strlen(buf);
    if (n + 1 < sizeof(buf)) { buf[n] = '\n'; buf[n + 1] = '\0'; }
    _port.print(buf);
    txMirror(buf);
  };
  emit(juicebox_build_amps_set(amps, buf, sizeof(buf)));
  emit(juicebox_build_offline_limit(amps, buf, sizeof(buf)));
  emit(juicebox_build_lock(!_enabled, buf, sizeof(buf)));
#ifdef JB_DEBUG
  LT_I("JBTX(~): ~AL/~OL=%d ~LK=%s", amps, _enabled ? "00(unlock)" : "01(lock)");
#endif
}

void JuiceBoxBackend::addStatusFields(JsonDocument &doc) const {
  doc["state_str"] = lite_evse_state_name(getState());
  if (_hw[0]) doc["hw"]       = _hw;
  if (_fw[0]) doc["fw"]       = _fw;
  if (_pv[0]) doc["protocol"] = _pv;
  if (_md[0]) doc["md"]       = _md;
  if (_wc[0]) doc["wc"]       = _wc;
  if (_wr[0]) doc["wr"]       = _wr;
  doc["line"] = _status.line;          // raw JB L field (semantics unknown per RE)

  // Refine the OpenEVSE numeric state for faults. web_server_lite set a blanket 8
  // (LiteEvseState::Error) — but the JuiceBox $WR code tells us WHICH fault, so map
  // it to the closest OpenEVSE code (No GND->7, GFI test->9, relay->8, lockout->6,
  // pilot->5). Keeps OpenEVSE-shaped consumers (UI label table, Home Assistant) from
  // mislabelling every fault "stuck relay". Exact text stays in `wr` for the UI.
  if (getState() == LiteEvseState::Error && _wr[0]) {
    int wrCode = juicebox_wr_code(_wr);
    if (wrCode >= 0) doc["state"] = juicebox_fault_openevse_state(wrCode);
  }

  // RX-health (diagnostic): comms_online mirrors isOnline(); rx_age_ms is time since the
  // last framed inbound message; rx_frames counts all framed messages; rx_overflow counts
  // RX-ring overflow events. Watch rx_frames climb (receiving) vs flatline (dropping), and
  // rx_overflow (UART can't keep up). Note md/wr/state above are STICKY (last value), so
  // these RX-health counters are the only true live-reception signal.
  doc["comms_online"] = isOnline();
  doc["rx_age_ms"]    = (uint32_t)(millis() - _lastRxMillis);
  doc["rx_frames"]    = (uint32_t)_rxFrames;
  doc["rx_overflow"]  = (uint32_t)_rxOverflows;
  // Raw USART0 hardware flags to confirm an RX wedge. IF error bits: RXOF=hw FIFO overflow,
  // FERR=framing error, PERR=parity. STATUS: RXENS=receiver enabled, RXBLOCK=RX blocked,
  // RXDATAV=byte available, RXFULL. If RX dies with FERR/RXOF latched in uart_if, the
  // receiver is stuck on an unhandled error (the RX IRQ never clears these). Diagnostic only.
  doc["uart_if"]     = (uint32_t)USART0->IF;
  doc["uart_status"] = (uint32_t)USART0->STATUS;
}
#endif
