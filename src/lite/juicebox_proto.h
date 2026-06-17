#pragma once
#include <stdint.h>
#include <stddef.h>
#include "lite_evse_state.h"

// static constexpr (not inline constexpr): the device toolchain builds at gnu++14,
// where `inline` variables warn. constexpr at namespace scope already has internal
// linkage, so each TU gets its own copy — no ODR clash.
static constexpr size_t JB_TYPE_LEN    = 2;
static constexpr size_t JB_MAX_PAYLOAD = 80;

// One decoded protocol frame: 2-char type + NUL-terminated payload.
struct JuiceBoxFrame {
  char     type[JB_TYPE_LEN + 1];
  char     payload[JB_MAX_PAYLOAD + 1];
  uint16_t len;                       // payload length
  char     start;                     // delimiter that began the frame: '$' or '~'
};

// Decoded $ES status fields (raw JB values, pre-normalization).
// Field semantics per SERIAL_PROTOCOL.md §2a (static decode of THIS unit's FW 100102):
//   state = "S" state-machine code (HEX on the wire; see JbStateCode)
//   line  = "L" — always 00 / unused
//   temp  = "T" — echo of the last TP-command value, NOT a temperature sensor
//   h     = "H" — quantized control-pilot voltage bucket (J1772 connection level)
//   amps  = "A" — live active current limit (echoes our setpoint)
//   power = "P" — control-pilot PWM duty, NOT watts
//   offline_limit = "F" — echo of the offline/fallback current limit (RAM 0x520),
//                   NOT a fault and NOT frequency. Faults come from S==0x05 + $MD/$WR.
struct JuiceBoxStatus {
  bool valid;
  int  state, line, temp, h, amps, power, offline_limit;
};

// $ES "S" state-machine codes. These are HEX on the wire (e.g. "S31" => 0x31).
// Per SERIAL_PROTOCOL.md §2a jump-table walk: exactly 7 valid codes.
enum JbStateCode {
  JB_S_INIT      = 0x00,  // init / reset (contactor open)
  JB_S_READY     = 0x01,  // ready — pilot setup
  JB_S_STANDBY   = 0x02,  // standby poll
  JB_S_FAULT     = 0x05,  // FAULT (forces contactor open) — see $MD/$WR for the code
  JB_S_IDLE      = 0x11,  // idle / connected poll (hub state)
  JB_S_PRECHARGE = 0x21,  // pre-charge (contactor opening toward charge)
  JB_S_CHARGING  = 0x31,  // CHARGING (contactor closed)
};

// Split a frame body (everything AFTER the leading '$') into type + payload.
// Handles "<TT><3hex>:<payload>" (e.g. $ES01C:...) and "<TT>:<payload>" (e.g. $MD:...).
bool juicebox_parse_frame_body(const char *raw, size_t n, JuiceBoxFrame &out);

// Incremental, framing-tolerant parser. A frame runs from a start delimiter ('$' or
// '~') to the next start delimiter or a CR/LF terminator (whichever comes first).
// Resyncs on '$'/'~'. feed() returns true and fills `out` exactly once per completed
// frame; out.start records which delimiter began it. The MCU's RX line-assembler
// accepts both '$' (EVSE protocol) and '~' (WiFi-module identity/version channel).
class JuiceBoxParser {
public:
  JuiceBoxParser() : _n(0), _started(false), _frameStart('$') {}
  bool feed(uint8_t b, JuiceBoxFrame &out);
  void reset() { _n = 0; _started = false; }
private:
  bool flush(JuiceBoxFrame &out);
  char   _raw[JB_MAX_PAYLOAD + 16];
  size_t _n;
  bool   _started;
  char   _frameStart;                 // delimiter of the in-progress frame
};

// Decode a $ES payload (e.g. "S00,L00,T00,H00,A00,P000,F00") into JuiceBoxStatus.
bool juicebox_parse_es(const char *payload, uint16_t len, JuiceBoxStatus &out);

// Map a raw JB S-field code (hex value, see JbStateCode) onto the canonical state.
// FIRM: 0x31/0x21 => Charging, 0x05 => Error. The idle/poll codes (00/01/02/11) map
// to NotConnected — S alone cannot tell "vehicle plugged, not charging" from "no
// vehicle"; distinguishing Connected needs the H pilot-voltage field (follow-up).
LiteEvseState juicebox_map_state(int raw);

// --- Frame checksum (custom hash @ ATmega flash 0x4998) -------------------------
// Verified byte-exact against 23 captured stock frames in both directions
// (2026-06-17 dual-UART tap). The hash covers the whole frame INCLUDING the leading
// $/~ prefix, up to (not including) the trailer. Reference: crc_codec.py.
uint16_t juicebox_crc(const char *data, size_t len);
// Encode a checksum as the 3-char base-0x3C trailer (out gets 3 chars + NUL).
void     juicebox_encode_trailer(uint16_t crc, char out[4]);

// Build a WiFi->MCU command frame "~<type><LLL hex>:<payload>:<trailer>:" into buf.
// The '~' prefix and trailer checksum are required for the MCU to accept the command
// (CONFIRMED on the wire 2026-06-17). Returns bytes written, 0 on overflow.
size_t juicebox_build_frame(const char *type, const char *payload, char *buf, size_t buflen);

// Build the host->MCU amps-set command "~AL002:NN:<trailer>:" (active current limit,
// the J1772 charge gate). `amps` is clamped to [0,79] (the MCU rejects >= 80).
// Returns bytes written, 0 on overflow.
size_t juicebox_build_amps_set(int amps, char *buf, size_t buflen);

// Build the host->MCU lock command "~LK002:NN:<trailer>:" — true=lock (stop charging,
// "01"), false=unlock (enable, "00"). The charge START/STOP gate stock drives, separate
// from the AL setpoint. Returns bytes written, 0 on overflow.
size_t juicebox_build_lock(bool locked, char *buf, size_t buflen);

// Build the host->MCU offline-limit command "~OL002:NN:<trailer>:" (fallback current if
// comms drop). `amps` clamped to [0,79]. Returns bytes written, 0 on overflow.
size_t juicebox_build_offline_limit(int amps, char *buf, size_t buflen);
