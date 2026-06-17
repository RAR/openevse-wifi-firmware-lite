#pragma once
// Live web consoles for lite (port-80 WebSocket), the streaming half of the
// nightshift Settings -> Terminal page. The full firmware gets this from
// Mongoose (server.on("/x/console$")->onFrame + sendAll); lite runs the
// synchronous LibreTiny WebServer, which has no WebSocket support, so this
// module supplies a minimal RFC6455 server:
//
//   - registers /evse/console + /debug/console as ordinary GET routes,
//   - upgrades the connection in-handler (101 + Sec-WebSocket-Accept) and
//     "steals" the WiFiClient (LwIPClient is shared_ptr-backed, so a held copy
//     keeps the lwIP socket open after WebServer drops _currentClient),
//   - frame-encodes outbound text to the parked sockets.
//
// The two streams are fed by direct taps: the JuiceBox backend mirrors the $/~
// wire (RX line-buffered + TX) into LITE_CONSOLE_EVSE, and firmware code calls
// lite_console_debugf() for LITE_CONSOLE_DEBUG. (The full firmware routes these
// through StreamSpy/Mongoose; the lite build strips both, so we tap directly —
// no shared-UART write hazard, since the debug path never touches Serial.)
// Both consoles are receive-only; inbound frames are drained/closed, never
// interpreted.
#ifdef OPENEVSE_LITE
#include <stddef.h>
#include <stdint.h>

class WebServer;

enum LiteConsoleKind {
  LITE_CONSOLE_DEBUG = 0,   // /debug/console  (firmware events)
  LITE_CONSOLE_EVSE  = 1,   // /evse/console   ($/~ inter-MCU traffic)
  LITE_CONSOLE_WS    = 2,   // /ws             (bidirectional: status push + {"ping"}/{"pong"})
  LITE_CONSOLE_KINDS = 3,
};

// Register the two console routes on `server`. Also arranges header collection
// for the WebSocket upgrade handshake. Call once, before server.begin().
void lite_console_begin(WebServer &server);

// Reap closed sockets and drain (ignore) any inbound frames. Call each web loop.
void lite_console_loop();

// Frame-encode `buf`/`len` as WS text and write to every live socket of `kind`.
// No-op when no console of that kind is connected (the common case), so the
// StreamSpy taps stay cheap.
void lite_console_broadcast(LiteConsoleKind kind, const uint8_t *buf, size_t len);

// True if at least one socket of `kind` is connected (lets taps skip work).
bool lite_console_has_client(LiteConsoleKind kind);

// printf-style firmware-event line to /debug/console (a '\n' is appended). Cheap
// when no debug console is open: it formats nothing if no client is connected.
void lite_console_debugf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#endif
