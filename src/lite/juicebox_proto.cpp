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
  out.start = _frameStart;   // set after parse_frame_body (which memset-zeroes out)
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
  // raw = $ES "S" field = the J1772 pilot state (see JbStateCode; HW-confirmed 2026-06-22).
  // Real faults ride $WR (not S). State D (0x05) maps to Error here; the backend then
  // refines the numeric OpenEVSE state to 4 (vent required) from the co-emitted $WR.
  switch (raw) {
    case JB_S_B:         return LiteEvseState::Connected;     // J1772 B: plugged, not charging
    case JB_S_C:                                              // J1772 C: charging
    case JB_S_PRECHARGE:                                      // legacy charging code (unseen)
    case JB_S_CHARGING:  return LiteEvseState::Charging;      // legacy charging code (unseen)
    case JB_S_D:         return LiteEvseState::Error;         // J1772 D: vent required (-> $WR refines to 4)
    case JB_S_A:                                              // J1772 A: no vehicle
    case JB_S_READY:     return LiteEvseState::NotConnected;  // transitional, no vehicle
    default:             return LiteEvseState::Unknown;
  }
}

int juicebox_wr_code(const char *wr) {
  if (!wr || wr[0] < '0' || wr[0] > '9') return -1;
  int code = 0;
  for (const char *p = wr; *p >= '0' && *p <= '9'; ++p) code = code * 10 + (*p - '0');
  return code;
}

int juicebox_fault_openevse_state(int wrCode) {
  // JuiceBox $WR code -> closest OpenEVSE state code (UI/HA label compatibility).
  // Pilot faults (004/005) and FW self-test (001) have no exact OpenEVSE code; the
  // exact text always rides `wr`. See the taxonomy in juicebox_proto.h.
  switch (wrCode) {
    case 3:   return 7;   // No GND                   -> no ground
    case 6:   return 9;   // GFI Auto Test Fail       -> GFCI self-test failed
    case 7:   return 8;   // Relay Stuck Closed       -> stuck relay
    case 8:   return 6;   // Ground Fault Int Lockout -> GFCI fault
    case 101: return 6;   // Ground Fault Int         -> GFCI fault
    case 102: return 8;   // Relay Stuck Open         -> stuck relay
    case 4:   return 5;   // Short Circuit Pilot      -> diode check (nearest pilot fault)
    case 5:   return 5;   // Pilot Signal Gen Fail    -> diode check (nearest pilot fault)
    case 105: return 4;   // Vehicle vent req         -> vent required (J1772 state D); HW-confirmed 2026-06-20
    case 1:   return 9;   // FW Self Tests Failed     -> GFCI self-test (nearest "self test")
    default:  return 8;   // unknown fault            -> generic fault slot
  }
}

bool JuiceBoxParser::feed(uint8_t b, JuiceBoxFrame &out) {
  if (b == '$' || b == '~') {
    bool ready = flush(out);   // close any in-progress frame (flush stamps its _frameStart)
    _started = true; _n = 0; _frameStart = (char)b;
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

// --- Frame checksum -------------------------------------------------------------
// Custom hash reversed from the ATmega image (routine @ flash 0x4998); NOT a standard
// polynomial CRC. Verified byte-exact against 23 captured stock frames in BOTH
// directions (2026-06-17 dual-UART tap; see device-configs .../crc_codec.py). The
// hash covers the whole frame INCLUDING the leading $/~ prefix, up to (not incl.) the
// trailer. The stock WiFi rejects frames with a wrong/missing trailer ("Inv EVSE msg
// ... CRC"), so our commands MUST carry it to be accepted.
static inline uint8_t jb_swapn(uint8_t b) { return (uint8_t)((b << 4) | (b >> 4)); }

uint16_t juicebox_crc(const char *data, size_t len) {
  uint16_t crc = 0;
  for (size_t i = 0; i < len; ++i) {
    uint8_t  d    = (uint8_t)data[i];
    uint16_t t    = (uint16_t)(crc >> 2);
    uint16_t u    = (uint16_t)(crc << 1);
    uint8_t  sulo = jb_swapn((uint8_t)(u & 0xFF));
    uint8_t  suhi = jb_swapn((uint8_t)(u >> 8));
    uint8_t  hi   = (uint8_t)((suhi & 0xF0) ^ (sulo & 0x0F));
    uint8_t  lo   = (uint8_t)(sulo & 0xF0);
    uint16_t val  = (uint16_t)(((uint16_t)hi << 8) | lo);
    val = (uint16_t)(val + t);
    int sd = (d & 0x80) ? (int)d - 256 : (int)d;   // data byte added as SIGNED int8
    val = (uint16_t)(val + sd);
    crc ^= val;
  }
  return crc;
}

// Encode the 16-bit checksum as the 3-char base-0x3C trailer (encoder @ flash 0xf3e),
// MSB first, with the +bit11/+bit5 injection the ATmega applies. out gets 3 chars + NUL.
void juicebox_encode_trailer(uint16_t crc, char out[4]) {
  out[0] = (char)(0x3C + ((crc >> 12) & 0x0F));
  out[1] = (char)(0x3C + ((crc >> 6)  & 0x3F) + ((crc >> 11) & 1));
  out[2] = (char)(0x3C + ( crc        & 0x3F) + ((crc >> 5)  & 1));
  out[3] = '\0';
}

// Build a WiFi->MCU command frame "~<TT><LLL>:<payload>:<trailer>:" into buf. The '~'
// prefix is the WiFi-module->safety-MCU direction (CONFIRMED on the wire 2026-06-17;
// stock sends ~AL/~OL/~LK, never '$' or 'SL'). The trailer is the required checksum.
// Returns bytes written, 0 on overflow.
size_t juicebox_build_frame(const char *type, const char *payload, char *buf, size_t buflen) {
  if (!type || strlen(type) != JB_TYPE_LEN) return 0;
  size_t plen = payload ? strlen(payload) : 0;
  if (plen > 0xFFF) return 0;
  // ~ TT LLL : payload : XXX :  + NUL
  size_t need = 1 + JB_TYPE_LEN + 3 + 1 + plen + 1 + 3 + 1 + 1;
  if (buflen < need) return 0;
  int bn = snprintf(buf, buflen, "~%s%03X:%s", type, (unsigned)plen, payload ? payload : "");
  if (bn <= 0 || (size_t)bn >= buflen) return 0;
  char tr[4];
  juicebox_encode_trailer(juicebox_crc(buf, (size_t)bn), tr);   // CRC covers "~TTLLL:payload"
  int fn = snprintf(buf + bn, buflen - (size_t)bn, ":%s:", tr);
  if (fn <= 0 || (size_t)(bn + fn) >= buflen) return 0;
  return (size_t)(bn + fn);
}

// Host->MCU set-active-limit command "~AL002:NN:<trailer>:". CONFIRMED "AL" by disasm
// (dispatch @0x14e0 -> active current limit RAM 0x519, the J1772 charge gate) AND on the
// wire (stock sends ~AL002:NN). NO 'SL' exists; the old guess set nothing. amps clamped
// to [0,79] (MCU rejects >= 80). Returns bytes written, 0 on overflow.
static const char JB_AMPS_CMD_TYPE[] = "AL";

size_t juicebox_build_amps_set(int amps, char *buf, size_t buflen) {
  if (amps < 0)  amps = 0;
  if (amps > 79) amps = 79;                 // MCU rejects >= 80
  char payload[3];
  snprintf(payload, sizeof(payload), "%02d", amps);
  return juicebox_build_frame(JB_AMPS_CMD_TYPE, payload, buf, buflen);
}

// Host->MCU lock command "~LK002:NN:<trailer>:" — 01=lock (stop), 00=unlock (enable).
// This is the charge START/STOP gate the stock module drives (HW-RE 2026-06-17), distinct
// from the AL current setpoint. Returns bytes written, 0 on overflow.
size_t juicebox_build_lock(bool locked, char *buf, size_t buflen) {
  return juicebox_build_frame("LK", locked ? "01" : "00", buf, buflen);
}

// Host->MCU offline-limit command "~OL002:NN:<trailer>:" — the fallback current the MCU
// applies if WiFi comms drop. amps clamped to [0,79]. Returns bytes written, 0 on overflow.
size_t juicebox_build_offline_limit(int amps, char *buf, size_t buflen) {
  if (amps < 0)  amps = 0;
  if (amps > 79) amps = 79;
  char payload[3];
  snprintf(payload, sizeof(payload), "%02d", amps);
  return juicebox_build_frame("OL", payload, buf, buflen);
}
