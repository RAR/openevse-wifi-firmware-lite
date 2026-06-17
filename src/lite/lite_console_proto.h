#pragma once
// Pure WebSocket (RFC6455) protocol helpers for the lite live consoles.
//
// Kept free of Arduino/WiFi deps so it builds + unit-tests in the native doctest
// env. The firmware glue (client handoff, broadcast) lives in lite_console.cpp;
// this file is just the wire math: the opening-handshake accept key and the
// server->client TEXT frame encoder. Server frames are NEVER masked (RFC6455
// 5.1), so the encoder emits no masking key.
#include <stddef.h>
#include <stdint.h>

// RFC6455 §1.3 opening handshake: Sec-WebSocket-Accept = base64(SHA1(key + GUID))
// where GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11". `client_key` is the value
// of the request's Sec-WebSocket-Key header. Writes a NUL-terminated 28-char
// accept string to `out` (needs >= 29 bytes). Returns 28 on success, 0 if
// out_cap is too small or client_key is null.
size_t lite_ws_accept_key(const char *client_key, char *out, size_t out_cap);

// Bytes the frame header occupies for a TEXT payload of `len`: 2 (len < 126),
// 4 (len <= 0xFFFF), or 10 (larger). Matches lite_ws_encode_text's framing.
size_t lite_ws_header_len(size_t len);

// Encode an unmasked server->client TEXT frame (FIN=1, opcode=0x1) for `payload`
// into `out`. Returns total bytes written (header + payload), or 0 if `out_cap`
// is too small to hold them.
size_t lite_ws_encode_text(const uint8_t *payload, size_t len, uint8_t *out, size_t out_cap);

// Encode ONLY the server->client TEXT frame header (FIN+opcode+length) for a
// payload of `len` bytes into out[0..10). Returns the header length (2/4/10).
// Lets a caller write the header then stream the payload in a second write — one
// WS frame, no large buffer, and (critically) no message splitting that would
// corrupt a JSON status push.
size_t lite_ws_encode_header(size_t len, uint8_t out[10]);

// One decoded inbound (client->server) frame. `payload` points into the caller's
// buffer; for a masked frame it has been unmasked in place. RFC6455 opcodes:
// 0x1 text, 0x2 binary, 0x8 close, 0x9 ping, 0xA pong.
struct LiteWsFrame {
  uint8_t  opcode;
  bool     fin;
  uint8_t *payload;
  size_t   payload_len;
};

// Decode one frame from in[0..len). Client frames are masked; the payload is
// unmasked IN PLACE (so `in` must be writable). Returns the total frame bytes
// consumed (header + payload), or 0 if in[0..len) does not yet hold a full frame.
size_t lite_ws_decode_frame(uint8_t *in, size_t len, LiteWsFrame *out);
