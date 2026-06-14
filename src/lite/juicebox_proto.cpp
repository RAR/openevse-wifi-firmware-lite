#include "juicebox_proto.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static bool is_hex(char c) {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

bool juicebox_parse_frame_body(const char *raw, size_t n, JuiceBoxFrame &out) {
  memset(&out, 0, sizeof(out));
  if (n < 3) return false;                  // need 2 type chars + at least ':'
  out.type[0] = raw[0];
  out.type[1] = raw[1];
  out.type[2] = '\0';

  const char *payload = nullptr;
  if (n >= 6 && is_hex(raw[2]) && is_hex(raw[3]) && is_hex(raw[4]) && raw[5] == ':') {
    payload = raw + 6;
    out.len = (uint16_t)strtol(raw + 2, nullptr, 16);   // strtol reads the 3 hex up to ':'
  } else if (raw[2] == ':') {
    payload = raw + 3;
  } else {
    const char *colon = strchr(raw + 2, ':');
    if (!colon) return false;
    payload = colon + 1;
  }

  size_t plen = strlen(payload);
  if (plen > JB_MAX_PAYLOAD) plen = JB_MAX_PAYLOAD;
  memcpy(out.payload, payload, plen);
  out.payload[plen] = '\0';
  if (out.len == 0) out.len = (uint16_t)plen;
  return true;
}

bool JuiceBoxParser::flush(JuiceBoxFrame &out) {
  if (!_started || _n == 0) { _started = false; _n = 0; return false; }
  _raw[_n] = '\0';
  bool ok = juicebox_parse_frame_body(_raw, _n, out);
  _started = false; _n = 0;
  return ok;
}

bool juicebox_parse_es(const char *payload, uint16_t len, JuiceBoxStatus &out) {
  memset(&out, 0, sizeof(out));
  if (!payload || len == 0) return false;

  const char *p   = payload;
  const char *end = payload + len;
  int fields = 0;

  while (p < end && *p) {
    char f = *p++;                       // field letter
    // The "S" state field is HEX on the wire (e.g. "S31" => 0x31); every other
    // field is decimal (SERIAL_PROTOCOL.md §2a). Parse with the right radix so a
    // charging state like 0x31 doesn't decimal-decode to 31 and miss the map.
    const int radix = (f == 'S') ? 16 : 10;
    bool neg = false;
    if (p < end && *p == '-') { neg = true; ++p; }   // decimal fields only; S is unsigned hex
    int v = 0; bool any = false;
    for (; p < end; ++p) {
      int d;
      if      (*p >= '0' && *p <= '9') d = *p - '0';
      else if (*p >= 'A' && *p <= 'F') d = *p - 'A' + 10;
      else if (*p >= 'a' && *p <= 'f') d = *p - 'a' + 10;
      else break;
      if (d >= radix) break;             // hex letter in a decimal field => not our digit
      v = v * radix + d; any = true;
    }
    if (!any) return false;              // letter with no number => malformed
    if (neg) v = -v;

    switch (f) {
      case 'S': out.state         = v; break;
      case 'L': out.line          = v; break;
      case 'T': out.temp          = v; break;
      case 'H': out.h             = v; break;
      case 'A': out.amps          = v; break;
      case 'P': out.power         = v; break;
      case 'F': out.offline_limit = v; break;
      default: break;                    // ignore unknown field letters
    }
    ++fields;
    if (p < end && *p == ',') ++p;       // step over separator
  }
  out.valid = fields > 0;
  return out.valid;
}

LiteEvseState juicebox_map_state(int raw) {
  // Hex state codes per SERIAL_PROTOCOL.md §2a (static decode of this unit's FW).
  // FIRM: 0x31 charging (contactor closed) + 0x21 pre-charge => Charging; 0x05 => fault.
  // Idle/poll codes => NotConnected: S cannot distinguish "plugged, idle" from "unplugged"
  // (the H pilot-voltage field does — promote to Connected in a follow-up).
  switch (raw) {
    case JB_S_CHARGING:
    case JB_S_PRECHARGE: return LiteEvseState::Charging;
    case JB_S_FAULT:     return LiteEvseState::Error;
    case JB_S_INIT:
    case JB_S_READY:
    case JB_S_STANDBY:
    case JB_S_IDLE:      return LiteEvseState::NotConnected;
    default:             return LiteEvseState::Unknown;
  }
}

bool JuiceBoxParser::feed(uint8_t b, JuiceBoxFrame &out) {
  if (b == '$') {
    bool ready = flush(out);   // close any in-progress frame
    _started = true; _n = 0;
    return ready;
  }
  if (b == '\r' || b == '\n') {
    return flush(out);
  }
  if (_started) {
    if (_n < sizeof(_raw) - 1) _raw[_n++] = (char)b;
    else { _started = false; _n = 0; }   // runaway → drop, resync on next '$'
  }
  return false;
}

size_t juicebox_build_frame(const char *type, const char *payload, char *buf, size_t buflen) {
  if (!type || strlen(type) != JB_TYPE_LEN) return 0;
  size_t plen = payload ? strlen(payload) : 0;
  if (plen > 0xFFF) return 0;
  size_t need = 1 + JB_TYPE_LEN + 3 + 1 + plen + 1;   // $ TT LLL : payload NUL
  if (buflen < need) return 0;
  int n = snprintf(buf, buflen, "$%s%03X:%s", type, (unsigned)plen, payload ? payload : "");
  return (n > 0 && (size_t)n < buflen) ? (size_t)n : 0;
}

// Host->MCU set-active-limit command. CONFIRMED "AL" by full disasm of the running
// ATmega image (SERIAL_PROTOCOL.md): dispatch arm @0x14e0 routes "AL" -> set-value
// handler (writes the active current limit RAM 0x519, the J1772 charge gate). There is
// NO 'S'-prefixed command anywhere in the firmware — the earlier "SL" was a wrong guess
// and was silently dropped at dispatch (it reset the comm watchdog but set nothing).
// "OL" is the offline-limit sibling (first char selects the target). Sent unsolicited.
static const char JB_AMPS_CMD_TYPE[] = "AL";

size_t juicebox_build_amps_set(int amps, char *buf, size_t buflen) {
  if (amps < 0)  amps = 0;
  if (amps > 79) amps = 79;                 // MCU rejects >= 80
  char payload[3];
  snprintf(payload, sizeof(payload), "%02d", amps);
  return juicebox_build_frame(JB_AMPS_CMD_TYPE, payload, buf, buflen);
}
