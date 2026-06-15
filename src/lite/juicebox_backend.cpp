#if defined(OPENEVSE_LITE) && defined(LITE_EVSE_BACKEND_JUICEBOX)
#include "juicebox_backend.h"
#include <string.h>
#include "lt_family.h"   // USART0 register access (em_device) — RX-wedge diagnostics

// Read-and-clear the USART0 RX ring-overflow flag (silabs core; declared in the core's
// SerialPrivate.h, which app code doesn't include). True => the RX IRQ had to drop bytes
// because the ring was full — i.e. we couldn't drain fast enough (TX/log contention).
extern bool SerialClass_getOverflow();

// Comm-watchdog reload is ~3000 ticks (~3 s, LIKELY per the Task 1 RE note);
// keepalive comfortably under it. Adjust once the timeout is HW-confirmed.
static const unsigned long JB_KEEPALIVE_INTERVAL_MS = 1000;
// Offline timeout must EXCEED the Atmel's slowest liveness frame. HW-observed
// 2026-06-13: in steady state the ONLY inbound frame is a $TP ping every ~63 s
// (metronomic; $ES/$MD only at boot/on-change). A 5 s timeout made isOnline()
// flap (true for 5 s after each ping, false for the other ~58 s). 90 s clears one
// ping interval with margin without flapping; a real dead controller still trips
// offline within ~90 s. Bump toward ~130 s if we ever need to ride out a missed ping.
static const unsigned long JB_OFFLINE_TIMEOUT_MS    = 90000;

void JuiceBoxBackend::begin() {
  _lastBeatMillis = millis();
}

void JuiceBoxBackend::loop() {
  JuiceBoxFrame f;
  while (_port.available() > 0) {
    int b = _port.read();
    if (b < 0) break;
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

  unsigned long now = millis();
  // Keepalive holds the comm watchdog. IMPORTANT (RE-confirmed): $SL is the only $S
  // keepalive and it ALWAYS sets the J1772 pilot current — there is NO current-neutral
  // heartbeat — so we advertise a deliberate safe limit (_chargeLimit, default 6 A floor),
  // never the MCU's reported max. Gated on _everRx so we don't transmit into a silent line.
  if (_everRx && (now - _lastBeatMillis) >= JB_KEEPALIVE_INTERVAL_MS) {
    sendKeepalive();
    _lastBeatMillis = now;
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
  if      (!strcmp(f.type, "ES")) {
    juicebox_parse_es(f.payload, f.len, _status);
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
  else if (!strcmp(f.type, "MD")) { copy_bounded(_md, sizeof(_md), f.payload, f.len); }
  else if (!strcmp(f.type, "WC")) { copy_bounded(_wc, sizeof(_wc), f.payload, f.len); }
  else if (!strcmp(f.type, "WR")) { copy_bounded(_wr, sizeof(_wr), f.payload, f.len); }
  // other types ignored this slice
}

void JuiceBoxBackend::sendKeepalive() {
  // Advertise the deliberate safe limit, not the MCU's reported max. juicebox_build_amps_set
  // clamps to [0,79]; the MCU further clamps <6 up to the 6 A J1772 floor.
  char buf[32];
  // Disabled => command 0 A. Per RE (SERIAL_PROTOCOL.md §4) an active limit < 6 A clears
  // the MCU's charge-enable gate (the real J1772 stop) — so we MUST keep sending (at 0),
  // not go silent: silence drops the MCU to offline mode where it keeps charging at the
  // persisted offline limit. Enabled => advertise the configured limit.
  int amps = _enabled ? _chargeLimit : 0;
  size_t n = juicebox_build_amps_set(amps, buf, sizeof(buf));
  if (n) {
    _port.write((const uint8_t *)buf, n);
#ifdef JB_DEBUG
    char safe[32]; jb_log_safe(safe, sizeof(safe), buf);
    LT_I("JBTX: %s", safe);   // what we send the Atmel (e.g. #AL002:24)
#endif
  }
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
