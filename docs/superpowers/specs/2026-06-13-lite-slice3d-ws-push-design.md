# Slice 3d: WebSocket `/ws` Status Push — Design

**Date:** 2026-06-13
**Branch:** `feature/juicebox-lite`
**Worktree:** `/home/rar/oevse/openevse-juicebox-lite`

## Context

Slices 3a/3b gave the firstof9/openevse Home Assistant integration the HTTP surface it needs
(`GET /status` poll + `/override` control). OpenEVSE firmware *also* exposes a WebSocket at
`/ws` that pushes the live status JSON, so clients (the OpenEVSE web GUI, dashboards) get
near-real-time updates without polling. This slice adds that push channel to the lite
firmware.

The vendored MongooseLite already compiles in the WebSocket server
(`MG_ENABLE_HTTP_WEBSOCKET = MG_ENABLE_HTTP`, on), and the listener already calls
`mg_set_protocol_http_websocket`, so a WS upgrade on the `:80` listener is handled by the
same event handler that serves HTTP. This is therefore a **glue-only** slice on top of the
existing server — no new socket, no new lib.

## Goal

A `/ws` WebSocket endpoint that pushes the exact `/status` JSON body to every connected
client at ~1 Hz (and immediately on connect), so a dashboard/HA sees live state without
polling — reusing the single `build_status_json` source of truth.

## OpenEVSE contract (what we mirror)

`ws://<host>/ws` — server→client push of the status JSON (~1 Hz on OpenEVSE). The lite unit
pushes the same body its `GET /status` returns. Commands stay on HTTP (`/override`,
`/config`, `/schedule`); the WS channel is **push-only** here.

## Design decisions (documented per the autonomy mandate)

**D1 — push-only, fixed ~1 Hz cadence, only when clients are connected.** Broadcast the full
`/status` JSON to all WS-flagged connections every `WS_PUSH_INTERVAL_MS` (1000 ms), plus an
immediate push on handshake so a new client gets instant state. We do **not** do field-level
change detection: `/status` contains `uptime` (ticks every second) and live `amp`/`power`, so
"changed" is nearly always true anyway; a 1 Hz cadence is simpler, faithful to OpenEVSE, and
bounded. The per-tick cost is skipped entirely when **no** WS client is connected (cheap
`MG_F_IS_WEBSOCKET` scan first; build the JSON only if ≥1 client). Inbound WS frames are
ignored (push-only; control stays on HTTP).

**D2 — reuse `build_status_json` verbatim.** The WS frame body IS the `GET /status` body
(same function), so the two never diverge. No second serializer.

**D3 — testable core = the cadence throttle.** The Mongoose WS plumbing (handshake event,
`mg_send_websocket_frame`, connection scan) is device-only and not native-testable, so it
stays thin. The one piece of real logic — "is it time to push again?" with millis-wrap
safety — is a pure `lite_ws_should_push(nowMs, lastPushMs, intervalMs)` unit, native-tested
(boundary + 32-bit wrap), mirroring `LiteClock::resyncDue`'s unsigned-subtraction pattern.

**D4 — exclude WS connections from the idle-connection reaper.** `web_server_lite_loop`
already reaps non-listening connections idle > 10 s (to clear wedged bodyless POSTs). A WS
client is long-lived and only receives (its `last_io_time` updates on each 1 Hz push, so it
*wouldn't* normally be reaped) — but to be safe against a push hiccup, add
`!(c->flags & MG_F_IS_WEBSOCKET)` to the reap predicate so live sockets are never culled.

**D5 — no config, no cap.** Always-on; cadence is a compile constant. No explicit client cap
beyond Mongoose's existing connection pool. The "skip when no clients" check keeps the idle
cost at one pointer walk per second.

## Architecture

### New pure unit: `src/lite/lite_ws.{h,cpp}`

```cpp
#include <stdint.h>

// True when it's time to push again: (nowMs - lastPushMs) >= intervalMs, using unsigned
// subtraction so a 32-bit millis() wrap (~49.7 d) yields a correct delta. Pure.
bool lite_ws_should_push(uint32_t nowMs, uint32_t lastPushMs, uint32_t intervalMs);
```

Implementation: `return (uint32_t)(nowMs - lastPushMs) >= intervalMs;`. No `OPENEVSE_LITE`
guard; `.cpp` added to `[env:native]` build_src_filter.

### Modified: `src/lite/web_server_lite.cpp`

- `#include "lite_ws.h"`; add `static const unsigned long WS_PUSH_INTERVAL_MS = 1000;` and
  `static unsigned long s_lastWsPushMs = 0;`.
- **`ev_handler`** currently early-returns on any `ev != MG_EV_HTTP_REQUEST`. Insert WS event
  handling *before* that return:
  - `MG_EV_WEBSOCKET_HANDSHAKE_DONE` → build `/status` and
    `mg_send_websocket_frame(nc, WEBSOCKET_OP_TEXT, body.c_str(), body.length())` (instant
    first frame for the new client); `return`.
  - `MG_EV_WEBSOCKET_FRAME` → `return;` (ignore inbound — push-only).
- **New `ws_broadcast_status()`** helper: build `/status` once, then
  `for (c = mg_next(&s_mgr, NULL); c; c = mg_next(&s_mgr, c)) if (c->flags & MG_F_IS_WEBSOCKET)
   mg_send_websocket_frame(c, WEBSOCKET_OP_TEXT, body.c_str(), body.length());`. Returns the
  number of clients it pushed to (so the caller can skip the build when zero — see below the
  cheaper ordering: first scan for any WS client, build only if found).
- **`web_server_lite_loop`** (after the schedule block, near the SNTP block): if
  `lite_ws_should_push(millis(), s_lastWsPushMs, WS_PUSH_INTERVAL_MS)`: scan for ≥1
  `MG_F_IS_WEBSOCKET` connection; if found, build `/status` once and broadcast to all WS
  clients; set `s_lastWsPushMs = millis()`. (Update `s_lastWsPushMs` even when no clients, so
  the next attempt is a cheap scan a second later.)
- **Idle reaper**: change the predicate to
  `!(c->flags & MG_F_LISTENING) && !(c->flags & MG_F_IS_WEBSOCKET) && (now - last_io) > IDLE`.
- Bump `LITE_FW_VERSION` `"lite-4"` → `"lite-4ws"`.

### `platformio.ini`

Add `+<lite/lite_ws.cpp>` to `[env:native]` build_src_filter.

## Data flow

```
client connects ws://host/ws
   -> MG_EV_WEBSOCKET_HANDSHAKE_DONE -> push build_status_json() once (instant state)
web_server_lite_loop each tick:
   lite_ws_should_push(now, lastPush, 1000)?  ── no ─> (cheap)
        │ yes
        ├─ any MG_F_IS_WEBSOCKET conn?  ── no ─> just bump lastPush
        └─ yes -> build_status_json() once -> mg_send_websocket_frame to each WS conn
   lastPush = now
GET /status (HTTP) unchanged — same body.
```

## Error handling / edge cases

- **No clients** → no JSON build (one pointer-walk per second); zero steady-state cost.
- **Client disconnects** → Mongoose drops the connection; the next broadcast simply doesn't
  see it (the scan is live each tick). No stale-handle use (we never cache connection
  pointers across ticks).
- **millis() wrap** → unsigned subtraction in `lite_ws_should_push` keeps the delta correct.
- **Reaper vs WS** → D4 guard keeps live WS sockets from being culled.
- **Backpressure** (slow client) → `mg_send_websocket_frame` buffers into the connection's
  send mbuf; at 1 Hz of a ~1 KB body this is negligible. Not addressed further (YAGNI); a
  pathologically stalled client is eventually closed by Mongoose's own send-buffer limits.

## Testing

### Native (doctest) — `test/test_lite_ws/`

- `lite_ws_should_push`: `now-last == interval` → true (boundary inclusive); `< interval` →
  false; `> interval` → true; `last == now` → false; **wrap**: `last = 0xFFFFFF00`,
  `now = 0x00000064` (100), `interval = 200` → delta 0x164 = 356 ≥ 200 → true; same with
  `interval = 500` → 356 < 500 → false.

### On-device (HW-validatable NOW — push runs over WiFi, ATmega-independent)

The bench unit (10.75.1.216) serves WiFi and `/status` regardless of the GFI fault, so the WS
push IS fully validatable: connect a WS client to `ws://10.75.1.216/ws`, confirm (a) an
immediate status frame on connect, (b) subsequent frames at ~1 Hz, (c) the body matches
`GET /status`, (d) the frame reflects live changes (e.g. `uptime` increments each push). This
is the second lite feature (after the LED) that bench-validates without a working ATmega.

## Files

- **Create:** `src/lite/lite_ws.{h,cpp}`, `test/test_lite_ws/`
- **Modify:** `src/lite/web_server_lite.cpp` (WS handshake push + loop broadcast + reaper
  guard + version bump)
- **Modify:** `platformio.ini` (`+<lite/lite_ws.cpp>` on `[env:native]`)
