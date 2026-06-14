# Slice 3d: WebSocket `/ws` Status Push Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A `/ws` WebSocket that pushes the `/status` JSON to connected clients at ~1 Hz (and immediately on connect), reusing `build_status_json`.

**Architecture:** A pure native-tested `lite_ws` cadence-throttle; thin Mongoose WS glue in `web_server_lite.cpp` (handshake push + per-loop broadcast + reaper guard). Push-only; control stays on HTTP.

**Tech Stack:** C++17, MongooseLite WebSocket (`mg_send_websocket_frame`, `MG_EV_WEBSOCKET_HANDSHAKE_DONE`, `MG_F_IS_WEBSOCKET`, `WEBSOCKET_OP_TEXT`), doctest (native), PlatformIO.

**Reference:** `docs/superpowers/specs/2026-06-13-lite-slice3d-ws-push-design.md`

---

## File Structure

- **Create** `src/lite/lite_ws.h` / `.cpp` — pure cadence throttle.
- **Create** `test/test_lite_ws/test_lite_ws.cpp` — doctest suite.
- **Modify** `src/lite/web_server_lite.cpp` — WS handshake push + broadcast + loop push + reaper guard + version bump.
- **Modify** `platformio.ini` — `+<lite/lite_ws.cpp>` on `[env:native]`.

---

### Task 1: Pure `lite_ws` throttle (TDD, native)

**Files:**
- Create: `src/lite/lite_ws.h`, `src/lite/lite_ws.cpp`
- Test: `test/test_lite_ws/test_lite_ws.cpp`
- Modify: `platformio.ini` (`[env:native]` build_src_filter)

- [ ] **Step 1: Header** — create `src/lite/lite_ws.h`:

```cpp
#pragma once
#include <stdint.h>

// True when it's time to push again: (nowMs - lastPushMs) >= intervalMs, using unsigned
// subtraction so a 32-bit millis() wrap (~49.7 d) yields a correct delta. Pure.
bool lite_ws_should_push(uint32_t nowMs, uint32_t lastPushMs, uint32_t intervalMs);
```

- [ ] **Step 2: Failing test** — create `test/test_lite_ws/test_lite_ws.cpp`:

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../src/lite/lite_ws.h"

TEST_CASE("should_push boundary") {
  CHECK_FALSE(lite_ws_should_push(0, 0, 1000));        // last==now -> false
  CHECK_FALSE(lite_ws_should_push(999, 0, 1000));      // < interval -> false
  CHECK(lite_ws_should_push(1000, 0, 1000));           // == interval -> true (inclusive)
  CHECK(lite_ws_should_push(1500, 0, 1000));           // > interval -> true
  CHECK_FALSE(lite_ws_should_push(1500, 1000, 1000));  // delta 500 -> false
}

TEST_CASE("should_push millis wrap-around") {
  // last just before the 32-bit wrap, now just after.
  uint32_t last = 0xFFFFFF00u;          // ~ -256
  uint32_t now  = 0x00000064u;          // 100  -> delta = 356
  CHECK(lite_ws_should_push(now, last, 200));        // 356 >= 200 -> true
  CHECK_FALSE(lite_ws_should_push(now, last, 500));  // 356 <  500 -> false
}
```

- [ ] **Step 3: Run, verify fail** — `pio test -e native -f test_lite_ws` → FAIL (undefined ref).

- [ ] **Step 4: Implement** — create `src/lite/lite_ws.cpp`:

```cpp
#include "lite_ws.h"

bool lite_ws_should_push(uint32_t nowMs, uint32_t lastPushMs, uint32_t intervalMs) {
  return (uint32_t)(nowMs - lastPushMs) >= intervalMs;
}
```

- [ ] **Step 5: Native build filter** — append `+<lite/lite_ws.cpp>` to the `[env:native]` `build_src_filter` line in `platformio.ini`.

- [ ] **Step 6: Run, verify pass** — `pio test -e native -f test_lite_ws` → PASS.

- [ ] **Step 7: Commit**

```bash
git add src/lite/lite_ws.h src/lite/lite_ws.cpp test/test_lite_ws/ platformio.ini
git commit -m "feat(lite): pure WebSocket push-cadence throttle (native-tested)"
```

---

### Task 2: `/ws` WebSocket glue in web_server_lite

**Files:**
- Modify: `src/lite/web_server_lite.cpp`

- [ ] **Step 1: Include + statics** — add `#include "lite_ws.h"` after `#include "lite_schedule.h"`. Then, next to the SNTP statics (search for `static const unsigned long SNTP_RETRY_MS`), add:

```cpp
static const unsigned long WS_PUSH_INTERVAL_MS = 1000;  // /ws status push cadence (~1 Hz)
static unsigned long       s_lastWsPushMs      = 0;
```

- [ ] **Step 2: Broadcast helper** — immediately BEFORE the `ev_handler` definition (after `build_status_json` is defined), add:

```cpp
// Push the current /status JSON to every connected WebSocket client. Cheap pre-scan first:
// build the body only when at least one WS client exists. Connection pointers are never
// cached across calls (each scan is live), so a client that dropped is simply not seen.
static int ws_broadcast_status()
{
  bool any = false;
  for (struct mg_connection *c = mg_next(&s_mgr, NULL); c != NULL; c = mg_next(&s_mgr, c)) {
    if (c->flags & MG_F_IS_WEBSOCKET) { any = true; break; }
  }
  if (!any) return 0;

  String body;
  build_status_json(body);
  int n = 0;
  for (struct mg_connection *c = mg_next(&s_mgr, NULL); c != NULL; c = mg_next(&s_mgr, c)) {
    if (c->flags & MG_F_IS_WEBSOCKET) {
      mg_send_websocket_frame(c, WEBSOCKET_OP_TEXT, body.c_str(), body.length());
      n++;
    }
  }
  return n;
}
```

- [ ] **Step 3: Handle WS events in `ev_handler`** — replace the opening of `ev_handler`:

```cpp
  (void)user_data;
  if (ev != MG_EV_HTTP_REQUEST) {
    return;
  }
```

with:

```cpp
  (void)user_data;

  // WebSocket (Slice 3d): push the current /status to a newly-connected client; ignore
  // inbound frames (the /ws channel is push-only — control stays on HTTP /override etc.).
  if (ev == MG_EV_WEBSOCKET_HANDSHAKE_DONE) {
    String body;
    build_status_json(body);
    mg_send_websocket_frame(nc, WEBSOCKET_OP_TEXT, body.c_str(), body.length());
    return;
  }
  if (ev == MG_EV_WEBSOCKET_FRAME) {
    return;
  }
  if (ev != MG_EV_HTTP_REQUEST) {
    return;
  }
```

- [ ] **Step 4: Per-loop push** — in `web_server_lite_loop`, AFTER the Slice-4 schedule block (the `if (s_mgr_ctrl && s_clock) { ... }` that ends with `s_lastSchedState = st;`) and BEFORE the `unsigned long nowMs = millis();` SNTP line, add:

```cpp
  // Slice 3d: push /status to WebSocket clients at ~1 Hz (only when any are connected;
  // ws_broadcast_status() does a cheap pre-scan and skips the JSON build otherwise).
  if (lite_ws_should_push(millis(), s_lastWsPushMs, WS_PUSH_INTERVAL_MS)) {
    ws_broadcast_status();
    s_lastWsPushMs = millis();
  }
```

- [ ] **Step 5: Spare WS sockets from the reaper** — in the reaper loop, change the predicate from:

```cpp
    if (!(c->flags & MG_F_LISTENING) &&
        (now - (double)c->last_io_time) > LITE_CONN_IDLE_SECS) {
```

to:

```cpp
    if (!(c->flags & MG_F_LISTENING) && !(c->flags & MG_F_IS_WEBSOCKET) &&
        (now - (double)c->last_io_time) > LITE_CONN_IDLE_SECS) {
```

- [ ] **Step 6: Version bump** — change `#define LITE_FW_VERSION "lite-4"` to `"lite-4ws"`.

- [ ] **Step 7: Build device env** — `pio run -e openevse_lite` → SUCCESS. Record flash %.
  If `mg_send_websocket_frame`/`MG_EV_WEBSOCKET_HANDSHAKE_DONE`/`MG_F_IS_WEBSOCKET` are
  unresolved, confirm `MG_ENABLE_HTTP_WEBSOCKET` is on in this build (it defaults to
  `MG_ENABLE_HTTP`, which is enabled) — no action expected.

- [ ] **Step 8: Commit**

```bash
git add src/lite/web_server_lite.cpp
git commit -m "feat(lite): /ws WebSocket push of /status (~1 Hz + on-connect); spare WS from reaper"
```

---

### Task 3: Full native suite + production build verification

**Files:** none (verification only)

- [ ] **Step 1: Full native suite** — `pio test -e native` → ALL PASS (incl. `test_lite_ws`).

- [ ] **Step 2: Production build** — `pio run -e openevse_lite` → SUCCESS; record flash % (was 24.1% / 51% of the 960 KB OTA slot after Slice 2 — see the flash-budget note; expect a tiny increase).

- [ ] **Step 3: Route/handler sanity** — `grep -n 'MG_EV_WEBSOCKET_HANDSHAKE_DONE\|ws_broadcast_status\|lite_ws_should_push\|MG_F_IS_WEBSOCKET' src/lite/web_server_lite.cpp` — confirm: handshake push present, broadcast helper present, loop push present, reaper guard present (the `MG_F_IS_WEBSOCKET` appears in both the broadcast scan and the reaper predicate).

- [ ] **Step 4:** Slice 3d is code-complete + native-tested. On-device HW validation (connect a WS client to `ws://10.75.1.216/ws`, confirm on-connect + ~1 Hz frames matching `/status`) is done by the controller after this task — it runs over WiFi, independent of the GFI-faulted ATmega.
```
