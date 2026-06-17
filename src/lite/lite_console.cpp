#ifdef OPENEVSE_LITE
#include "lite_console.h"
#include "lite_console_proto.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// Parked WebSocket sockets. The nightshift UI opens one console at a time (modal),
// but a stale socket can linger until its close is noticed, so a few slots avoid
// dropping a fresh connection while an old one drains. WiFiClient (LwIPClient) is
// shared_ptr-backed: copying into the slot keeps the lwIP socket alive after the
// WebServer releases its own _currentClient reference at end of handleClient().
#define LITE_CONSOLE_MAX 4

namespace {
struct Slot {
  WiFiClient client;
  uint8_t    kind = 0;
  bool       used = false;
};
Slot       s_slots[LITE_CONSOLE_MAX];
WebServer *s_srv = nullptr;

// Headers we need the WebServer to retain so the upgrade handshake can read them.
const char *kWsHeaders[] = { "Upgrade", "Sec-WebSocket-Key", "Sec-WebSocket-Version", "Connection" };

void park(WiFiClient &c, LiteConsoleKind kind) {
  for (int i = 0; i < LITE_CONSOLE_MAX; i++) {
    if (!s_slots[i].used) {
      s_slots[i].client = c;
      s_slots[i].kind   = (uint8_t)kind;
      s_slots[i].used   = true;
      return;
    }
  }
  // Table full: evict the oldest (slot 0) so a fresh console always connects.
  s_slots[0].client.stop();
  s_slots[0].client = c;
  s_slots[0].kind   = (uint8_t)kind;
  s_slots[0].used   = true;
}

// Common upgrade path for both routes. On a non-WebSocket request (no key),
// answers 426 so a stray GET doesn't hang; otherwise completes the handshake
// and steals the socket WITHOUT calling server.send() (so the WebServer leaves
// _chunked false and _finalizeResponse() is a no-op — the socket stays open).
void do_upgrade(LiteConsoleKind kind) {
  if (!s_srv) return;
  String key = s_srv->header("Sec-WebSocket-Key");
  if (key.length() == 0) {
    s_srv->send(426, "text/plain", "Upgrade Required");
    return;
  }
  char accept[32];
  if (lite_ws_accept_key(key.c_str(), accept, sizeof(accept)) == 0) {
    s_srv->send(400, "text/plain", "bad ws key");
    return;
  }
  WiFiClient c = s_srv->client();
  String resp = F("HTTP/1.1 101 Switching Protocols\r\n"
                  "Upgrade: websocket\r\n"
                  "Connection: Upgrade\r\n"
                  "Sec-WebSocket-Accept: ");
  resp += accept;
  resp += F("\r\n\r\n");
  c.write((const uint8_t *)resp.c_str(), resp.length());
  park(c, kind);
}

void handle_evse_console()  { do_upgrade(LITE_CONSOLE_EVSE); }
void handle_debug_console() { do_upgrade(LITE_CONSOLE_DEBUG); }
void handle_ws()            { do_upgrade(LITE_CONSOLE_WS); }

// Write one WS TEXT frame to `c` — header then payload, so a frame of any size
// goes out whole (never split into multiple WS messages, which would corrupt a
// JSON status push). Returns false if the socket choked mid-write.
bool send_text(WiFiClient &c, const uint8_t *p, size_t n) {
  uint8_t hdr[10];
  size_t hl = lite_ws_encode_header(n, hdr);
  if (c.write(hdr, hl) != hl) return false;
  return n == 0 || c.write(p, n) == n;
}

// Substring search over a non-NUL-terminated buffer (the UI's ping is {"ping":1}).
bool contains(const uint8_t *p, size_t n, const char *needle) {
  size_t m = strlen(needle);
  if (m == 0 || n < m) return false;
  for (size_t i = 0; i + m <= n; i++) {
    size_t j = 0;
    while (j < m && p[i + j] == (uint8_t)needle[j]) j++;
    if (j == m) return true;
  }
  return false;
}

// Read + handle one pass of inbound frames on a /ws socket: answer the UI's
// {"ping":1} with {"pong":1}, honor a close. Returns false if the socket should
// be dropped (close received / peer gone). Frames are assumed to arrive whole in
// one read pass (the UI's control/ping frames are tiny); a partial tail is left
// for next pass only if it never completes, in which case it's harmless to drop.
bool service_ws(WiFiClient &c) {
  uint8_t buf[256];
  size_t  n = 0;
  while (c.available() && n < sizeof(buf)) { int b = c.read(); if (b < 0) break; buf[n++] = (uint8_t)b; }
  size_t off = 0;
  while (off < n) {
    LiteWsFrame fr;
    size_t used = lite_ws_decode_frame(buf + off, n - off, &fr);
    if (!used) break;                       // incomplete frame; wait for more
    off += used;
    if (fr.opcode == 0x8) return false;      // close
    if (fr.opcode == 0x1 && contains(fr.payload, fr.payload_len, "ping")) {
      static const char kPong[] = "{\"pong\":1}";
      if (!send_text(c, (const uint8_t *)kPong, sizeof(kPong) - 1)) return false;
    }
    // 0x9 control-ping / 0xA pong / 0x2 binary: ignored (the UI sends none)
  }
  return true;
}
} // namespace

void lite_console_begin(WebServer &server) {
  s_srv = &server;
  server.collectHeaders(kWsHeaders, sizeof(kWsHeaders) / sizeof(kWsHeaders[0]));
  server.on("/evse/console",  handle_evse_console);
  server.on("/debug/console", handle_debug_console);
  server.on("/ws",            handle_ws);   // status push + ping/pong (replaces polling when up)
}

bool lite_console_has_client(LiteConsoleKind kind) {
  for (int i = 0; i < LITE_CONSOLE_MAX; i++)
    if (s_slots[i].used && s_slots[i].kind == (uint8_t)kind && s_slots[i].client.connected())
      return true;
  return false;
}

void lite_console_loop() {
  for (int i = 0; i < LITE_CONSOLE_MAX; i++) {
    if (!s_slots[i].used) continue;
    if (!s_slots[i].client.connected()) {
      s_slots[i].client.stop();
      s_slots[i].used = false;
      continue;
    }
    if (s_slots[i].kind == (uint8_t)LITE_CONSOLE_WS) {
      // /ws is bidirectional: decode inbound, answer pings, honor close.
      if (!service_ws(s_slots[i].client)) { s_slots[i].client.stop(); s_slots[i].used = false; }
    } else {
      // Receive-only console: drain inbound (browser close/ping) so the RX ring
      // never wedges. A real close drops connected() above on the next pass.
      while (s_slots[i].client.available()) s_slots[i].client.read();
    }
  }
}

void lite_console_broadcast(LiteConsoleKind kind, const uint8_t *buf, size_t len) {
  if (!buf || len == 0) return;
  for (int i = 0; i < LITE_CONSOLE_MAX; i++) {
    if (!s_slots[i].used || s_slots[i].kind != (uint8_t)kind) continue;
    if (!s_slots[i].client.connected()) { s_slots[i].client.stop(); s_slots[i].used = false; continue; }
    if (!send_text(s_slots[i].client, buf, len)) { s_slots[i].client.stop(); s_slots[i].used = false; }
  }
}

void lite_console_debugf(const char *fmt, ...) {
  if (!fmt || !lite_console_has_client(LITE_CONSOLE_DEBUG)) return;  // format nothing if unwatched
  char buf[192];
  va_list ap; va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
  va_end(ap);
  if (n <= 0) return;
  if (n > (int)sizeof(buf) - 2) n = sizeof(buf) - 2;  // clamp on truncation
  buf[n++] = '\n';
  lite_console_broadcast(LITE_CONSOLE_DEBUG, (const uint8_t *)buf, (size_t)n);
}
#endif
