# JuiceBox-lite Web Server Migration: MongooseLite → LibreTiny WebServer

**Status:** Approved (design) — 2026-06-15
**Repo:** `/home/rar/oevse/openevse-wifi-firmware-lite` (branch `main`)

## Goal

Replace the lite firmware's MongooseLite-based web server with LibreTiny's native
`WebServer` over the thread-safe lwIP **socket** API, ending a recurring class of
`NO_SYS=0` cross-thread bugs in the mongoose lwIP port. Same HTTP surface; the only
external change is dropping the WebSocket `/ws` push in favor of GUI polling of
`GET /status`.

## Why (root cause being escaped)

MongooseLite vendors an ESP-RTOS-SDK lwIP port (`mg_lwip_*`) that talks to lwIP's
**raw API** from the mongoose poll task while a separate tcpip thread runs RX and
timers. lwIP here is `NO_SYS=0` + `LWIP_TCPIP_CORE_LOCKING=1`. This produced three
threading bugs in sequence:

1. **Open-AP `start_ap` 0x21** — fixed in the LibreTiny driver (`7abf7a5`); the
   firmware also secures the setup AP with WPA2 (`9fdaad8`), kept.
2. **`rx_chain` pbuf double-free on scan** — the port's `mgos_lock`/`mgos_unlock`
   compiled to no-ops; backed with a real recursive mutex (`ee5f42a`). Fixed the
   scan crash.
3. **PCB-list corruption serving the web UI** — the port marshals lwIP calls onto
   the tcpip thread via `tcpip_callback`, but the **send/close path races**: serving
   the 21–26 KB gzip bundle queues many `tcp_write`/`tcp_output` callbacks, then
   `MG_F_SEND_AND_CLOSE` queues `tcp_close` for the same pcb; `tcp_close` can free
   the pcb while pending `tcp_output(pcb)` callbacks still reference it →
   use-after-free → a pcb lands on the wrong list (`tcp_slowtmr: active pcb->state
   != LISTEN` assert) and a freed/reused slot's function pointer is called →
   HardFault (INVSTATE, PC in the lwIP static-pool BSS region).

A separate symptom — `vApplicationMallocFailedHook` then OOM corruption when serving
the page — stemmed from the 64 KB FreeRTOS heap (LibreTiny `__wrap`s all malloc onto
`heap_4`). That was addressed by bumping `configTOTAL_HEAP_SIZE` to 256 KB
(fork `#ifndef`-guarded at `14402af`; firmware sets `-DconfigTOTAL_HEAP_SIZE=262144`).
Bug #3 persisted with the larger heap, proving it is a memory-safety bug, not
starvation.

LibreTiny's `WebServer` is built on `WiFiServer`/`WiFiClient` = lwIP **sockets**
(`LWIP_SOCKET=1`, `LWIP_NETCONN=1`, both already enabled). The socket API is
thread-safe by design (netconn → tcpip thread via mailboxes), so it eliminates the
entire raw-API-from-the-wrong-thread bug class. Socket send is streamed/bounded, so
the 26 KB single-mbuf allocation also disappears.

## Architecture

**Transport seam swap, logic untouched.** The handler *logic* (status JSON build,
POST apply, config/schedule/override/divert/shaper, provisioning decisions) already
lives in pure units with native doctest coverage. Only the transport changes.

- Replace the mongoose `mg_mgr` + `ev_handler` with a single module-static
  `WebServer s_server(80)`.
- `web_server_lite_begin(...)`: load config (unchanged), register routes, then
  `s_server.begin()`.
- `web_server_lite_loop()`: `s_server.handleClient()` plus the existing deferred
  reboot / D3 STA-retry checks.

All I/O goes through sockets; no raw lwIP calls, no `tcpip_callback` lifecycle, no
giant mbuf.

## Components / file structure

- `src/lite/web_server_lite.cpp` — **rewritten transport** (routes + thin glue),
  retaining the existing pure-ish helpers (`build_status_json`, `status_post_apply`,
  config/schedule/override JSON builders + appliers). One file; no new module needed.
- `src/lite/web_server_lite.h` — unchanged public surface
  (`web_server_lite_begin/loop/set_ap_mode/in_ap_mode`).
- `src/lite/lite_ws.{h,cpp}` — **deleted** (WebSocket push removed).
- `lib/MongooseLite/` — **removed from the build** (and may be deleted from the repo).
- `platformio.ini` `[env:openevse_lite]` — drop `-I lib/MongooseLite`, `MG_ENABLE_*`,
  `CS_PLATFORM`, `MG_ENABLE_SNTP`; keep `-DconfigTOTAL_HEAP_SIZE=262144`. Add the
  WebServer/WiFiServer/WiFiClient libs if not auto-discovered (they are LibreTiny
  built-ins, normally on the include path).

## Routes (1:1 with current behavior)

| Route | Method | Handler behavior |
|---|---|---|
| `/` | GET | gzip bundle for the current mode (`s_apMode ? SETUP_HTML_GZ : INDEX_HTML_GZ`), binary-safe: `sendHeader("Content-Encoding","gzip")` + `setContentLength(len)` + `send(200,"text/html","")` + `sendContent((const char*)body, len)` |
| `/status` | GET | `build_status_json` → `send(200,"application/json",body)` |
| `/status` | POST | `status_post_apply(arg("plain"))` → `{"msg":"OK"}` |
| `/config` | GET/POST | existing config get/apply logic via `arg()` |
| `/override` | as today | existing override logic |
| `/schedule`, `/schedule/<id>` | as today | base route + subpath via `UriBraces("/schedule/{}")` (fallback: `onNotFound` prefix-match on `uri()`) |
| `/scan` | GET | `WiFi.scanNetworks()` (sync) → JSON `[{ssid,rssi,enc}]`, `scanDelete()` |
| `/connect` | GET | `arg("ssid")`/`arg("pass")` (WebServer URL-decodes) → save → `200 {"msg":"OK"}` → deferred reboot; `400` no ssid, `500` save fail |
| (any other) | — | `onNotFound` → `404` |

WebServer's `Parsing.cpp` URL-decodes query args, matching the old
`mg_get_http_var` behavior — store decoded values directly (no second decode).

## Data flow

1. `loop()` → `web_server_lite_loop()` → `s_server.handleClient()` accepts one client,
   parses the request, dispatches to the registered route handler.
2. Handlers call the existing pure logic and `s_server.send(...)`/`sendContent(...)`.
   Socket writes are bounded by `TCP_SND_BUF`; large bodies stream over successive
   socket writes inside `handleClient` — no 26 KB heap spike.
3. `/connect`: handler saves creds, sends `200`, sets `s_rebootPending` +
   `s_rebootAtMs`; `web_server_lite_loop()` reboots after the response flushes.

## Time sync (SNTP)

Mongoose provided SNTP (`mg_sntp_get_time`, `MG_ENABLE_SNTP`). Replace it with the
**lwIP built-in SNTP app** (`apps/sntp`), which ships in the gecko-sdk lwIP but is
not currently enabled/compiled. This requires a one-time **libretiny-fork** change
(`lwipopts.h`: `SNTP_SUPPORT=1`, `SNTP_SERVER_DNS=1`, a `SNTP_SET_SYSTEM_TIME(sec)`
macro hooking a firmware-provided `lite_sntp_set_system_time(unsigned)`; plus adding
`src/apps/sntp/sntp.c` to the lwIP build). The firmware calls `sntp_setoperatingmode`
/`sntp_setservername`/`sntp_init` in `web_server_lite_begin`, and the hook stashes the
epoch (set on the tcpip thread) for `web_server_lite_loop` to apply via
`LiteClock::setEpoch` on the main task. The transport migration does **not** depend on
this — it builds with the clock unsynced; SNTP wires in once the fork change is pulled.

## WebSocket removal + GUI coordination

- Firmware: delete `lite_ws.*`, the handshake, and all broadcast calls. No `/ws` route.
- GUI (UI agent): the dashboard (`App.svelte` → `WebSocket.svelte`) switches from
  `new WebSocket('/ws')` to polling `GET /status` (~1.5 s). The status store already
  accepts the polling feed, so this is a localized change.
- Latency: push ~1 Hz → poll ~1.5 s. Acceptable for EVSE status (revisit push later
  if wanted — SSE or a WS lib).
- Also relay (still pending): the setup AP is now **WPA2, password `openevse`**
  (SSID `OpenEVSE-Lite-<6hex>`); update any "open AP" copy.

## Error handling

- `400` missing ssid, `500` save failure, `404` not-found — unchanged.
- Connection accept/timeout/teardown handled by `WebServer`/sockets (no app code).
- A `/scan` with a client associated must not crash (the original Bug-2 scenario);
  sockets isolate the scan from the connection's I/O.

## Testing

- **Native doctest:** unchanged — it covers the pure units
  (`lite_provision`, `lite_charge_policy`, `lite_schedule`, `lite_divert`,
  `lite_shaper`, `lite_openevse_compat`, etc.), not the transport. Must stay green.
- **Device build:** `pio run -e openevse_lite` clean; note flash delta (mongoose
  removed → smaller).
- **HW round-trip (the gauntlet that has been failing):**
  1. Join `OpenEVSE-Lite-<id>` AP (WPA2 / `openevse`).
  2. `GET /` loads the full setup page — no malloc-fail, no HardFault.
  3. Rescan several times while associated — list returns each time, no crash
     (Bug-2 scenario).
  4. `/connect` → saves → reboots into STA.
  5. Dashboard loads at the new IP and updates via `/status` polling.
  6. Power-cycle → creds persist (KVS).

## Out of scope / revisit later

- WebSocket/SSE live push (polling for now).
- Deleting `lib/MongooseLite` from the repo (can remove from build first, delete in a
  follow-up to keep the diff reviewable).
- Concurrent-client performance (single-client setup/dashboard is the norm).

## Risks

- `UriBraces`/`UriRegex` availability for `/schedule/<id>` — fallback is an
  `onNotFound` prefix match on `uri()`; both are in-tree.
- WebServer default request-buffer/timeout behavior for the POST `/status` body —
  verify `arg("plain")` returns the full body; collect headers if needed via
  `collectHeaders`.
- `WiFi.scanNetworks()` is still synchronous and briefly blocks `handleClient`
  (~750 ms) — acceptable on the setup page; async-scan remains a future option.
