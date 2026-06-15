# Lite Web Server Migration (MongooseLite → LibreTiny WebServer) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the lite firmware's MongooseLite web server with LibreTiny's `WebServer` over the thread-safe lwIP socket API, ending the recurring `NO_SYS=0` cross-thread bug class.

**Architecture:** Transport-seam swap. All handler *logic* (status/config/schedule/override JSON build + apply, divert/shaper/override/schedule control loop) is kept verbatim; only the mongoose transport is replaced — HTTP via `WebServer` routes, time sync via the lwIP built-in SNTP app, and GUI `/status` polling (no `/ws` WebSocket).

**Tech Stack:** LibreTiny (silabs-efm32gg11), Arduino `WebServer`, lwIP `apps/sntp`, ArduinoJson 6.20.1, FreeRTOS, doctest (native).

**Reference:** spec `docs/superpowers/specs/2026-06-15-lite-webserver-migration-design.md`.

**Repo/branch:** `/home/rar/oevse/openevse-wifi-firmware-lite`, branch `main`. Commit author `Andrew Rankin <andrewrankin@gmail.com>`; NO Claude attribution.

**Build/test commands:**
- Native suite: `pio test -e native`
- Device build: `pio run -e openevse_lite`
- Flash (user/SWD): `pio run -e openevse_lite -t upload`

---

## File Structure

- **Modify** `src/lite/web_server_lite.cpp` — rewrite the transport section (routes + SNTP wiring); keep all pure logic functions verbatim.
- **Delete** `src/lite/lite_ws.h`, `src/lite/lite_ws.cpp` — WebSocket push removed.
- **Modify** `platformio.ini` — `[env:openevse_lite]` drop the mongoose `-I`/`MG_*`/`CS_PLATFORM` flags; `[env:native]` drop `lite_ws.cpp`.
- **Not touched:** `src/lite/web_server_lite.h` (public surface unchanged), `src/lite/main_lite.cpp` (no mongoose refs). `lib/MongooseLite/` is dropped from the build but left on disk (physical deletion is an out-of-scope follow-up to keep this diff reviewable).
- **Libretiny fork (separate repo, libretiny agent):** enable lwIP SNTP — see Prerequisite. No `lite_ntp` unit is created (lwIP's SNTP app does the parsing).

---

## Prerequisite (libretiny fork — handed to the libretiny agent)

Time sync uses the gecko-sdk lwIP SNTP app, which ships but is **not enabled or
compiled** in the silabs port. The fork (`RAR/libretiny` branch
`feature/silabs-efm32gg11-ota`) needs:

1. **`cores/silabs-efm32gg11/base/config/lwipopts.h`** — add:
   ```c
   #define SNTP_SUPPORT     1
   #define SNTP_SERVER_DNS  1
   #define SNTP_SET_SYSTEM_TIME(sec) lite_sntp_set_system_time(sec)
   #ifdef __cplusplus
   extern "C" {
   #endif
   void lite_sntp_set_system_time(unsigned int sec);   // provided by the firmware
   #ifdef __cplusplus
   }
   #endif
   ```
2. **Add `src/apps/sntp/sntp.c`** (gecko-sdk lwIP) to the silabs lwIP build source
   list so it is compiled/linked (its include path `lwip/apps/sntp.h` is already on
   the lwIP include path).

Once pushed, the firmware `pio pkg update -e openevse_lite` pulls it. **Task 1
(the transport migration) does NOT depend on this** — it builds and fixes the
crashes with the clock simply unsynced. **Task 3 wires SNTP and requires this.**

---

## Task 1: Migrate the web server transport to `WebServer`

**Files:**
- Modify: `src/lite/web_server_lite.cpp` (rewrite the transport; keep pure logic verbatim)
- Modify: `platformio.ini` (`[env:openevse_lite]` `build_flags`)

This replaces the mongoose transport in one coherent change (intermediate states
do not compile). NTP is **omitted** here (clock left unsynced; added in Task 3).

**KEEP VERBATIM (unchanged — touch no mongoose types):**
`sched_state_from_str`, `sched_state_str`, `schedule_get_json`, `schedule_parse`,
`override_state_str`, `override_apply`, `override_clear`, `override_parse`,
`override_get_json`, `status_post_apply`, `build_status_json`, `config_json`, and
the module-static state for `s_apMode`, `s_rebootPending`, `s_rebootAtMs`,
`s_mgr_ctrl`, `s_clock`, `s_totals`, `s_sntpHost`, the divert/shaper/override/
schedule state blocks, `s_cfg`, `s_divertCfg`, `s_shaperCfg`, `s_feed`, plus the
accessors `web_server_lite_set_ap_mode` / `web_server_lite_in_ap_mode`.

**REMOVE:** `schedule_id_from_uri`, the old `(mg_connection*, http_message*)`
handlers, `sntp_ev_handler`, `ws_broadcast_status`, `ev_handler`, the
`mgos_lock`/`mgos_unlock`/`s_mgLock` block, `static struct mg_mgr s_mgr;`,
`s_lastWsPushMs`, `WS_PUSH_INTERVAL_MS`, `LITE_CONN_IDLE_SECS`,
`s_lastSntpAttemptMs`, `SNTP_RETRY_MS`, and the mongoose calls in begin()/loop().

- [ ] **Step 1: Replace the include block + mgos_lock block**

Replace everything from `#include <Arduino.h>` through the `mgos_unlock`
definition (the mongoose include-order comment, `mongoose.h`, `FreeRTOS.h`,
`semphr.h`, `lite_ws.h`, and the `s_mgLock`/`mgos_lock`/`mgos_unlock` lines) with:

```cpp
#ifdef OPENEVSE_LITE
#include <Arduino.h>
#include <ArduinoJson.h>
#include <stdlib.h>
#include <WiFi.h>
#include <WebServer.h>
#include "uri/UriBraces.h"

#include "lite_evse_backend.h"
#include "lite_evse_manager.h"
#include "lite_clock.h"
#include "lite_energy_totals.h"
#include "manual.h"
#include "web_server_lite.h"
#include "espal_lite.h"
#include "lite_config_store.h"
#include "lite_charge_policy.h"
#include "lite_override.h"
#include "lite_schedule.h"
#include "lite_openevse_compat.h"
#include "lite_feed.h"
#include "lite_divert.h"
#include "lite_shaper.h"
#include "lite_provision.h"
#include "web_ui_lite.h"

// Reported as OpenEVSE `firmware`/`version` so the HA integration shows a value.
#ifndef LITE_FW_VERSION
#define LITE_FW_VERSION "lite-web1"
#endif

// Manual override is defined in main_lite.cpp; reached here for /override + status.
extern ManualOverride manual;

// HTTP server over LibreTiny WiFiServer/WiFiClient = thread-safe lwIP sockets
// (no raw lwIP from the poll task; no 26 KB single mbuf). One client per
// handleClient() pass — fine for the single-client setup/dashboard.
static WebServer s_server(80);
```

Keep the existing `s_apMode/...` statics and the
`web_server_lite_set_ap_mode/in_ap_mode` accessors that follow (verbatim). Delete
the `s_lastSntpAttemptMs` / `SNTP_RETRY_MS` / `s_lastWsPushMs` /
`WS_PUSH_INTERVAL_MS` statics (SNTP is re-added in Task 3; WS push is gone).

- [ ] **Step 2: Replace `schedule_id_from_uri` + `handle_schedule`**

```cpp
// GET (list) / POST (upsert) / DELETE?id= on the base path.
static void handle_schedule() {
  int code = 200; String body;
  if (s_server.method() == HTTP_POST) {
    String b = s_server.arg("plain");
    LiteScheduleEvent e;
    if (schedule_parse(b.c_str(), b.length(), e, code)) {
      if (lite_schedule_upsert(s_schedule, e)) {
        bool saved = lite_config_save_schedule(s_schedule); s_scheduleVersion++;
        code = saved ? 201 : 503;
        StaticJsonDocument<64> r; r["id"] = e.id; serializeJson(r, body);
      } else { code = 507; body = "{\"msg\":\"Schedule full\"}"; }
    } else { body = "{\"msg\":\"Bad schedule event\"}"; }
  } else if (s_server.method() == HTTP_DELETE) {
    uint32_t id = 0; String q = s_server.arg("id");
    if (q.length() > 0) id = (uint32_t)strtoul(q.c_str(), NULL, 10);
    if (id != 0 && lite_schedule_remove(s_schedule, id)) {
      lite_config_save_schedule(s_schedule); s_scheduleVersion++;
      body = "{\"msg\":\"Deleted\"}";
    } else { code = 404; body = "{\"msg\":\"Not found\"}"; }
  } else {
    schedule_get_json(body);   // GET
  }
  s_server.send(code, "application/json", body);
}

// DELETE /schedule/<id> (path form) — id from the UriBraces path arg.
static void handle_schedule_del_path() {
  int code = 200; String body;
  uint32_t id = (uint32_t)strtoul(s_server.pathArg(0).c_str(), NULL, 10);
  if (id != 0 && lite_schedule_remove(s_schedule, id)) {
    lite_config_save_schedule(s_schedule); s_scheduleVersion++;
    body = "{\"msg\":\"Deleted\"}";
  } else { code = 404; body = "{\"msg\":\"Not found\"}"; }
  s_server.send(code, "application/json", body);
}
```

- [ ] **Step 3: Replace `handle_override`**

```cpp
static void handle_override() {
  int code = 200; String body;
  String qs = s_server.arg("state");   // legacy bodyless convenience (any method)
  if (qs.length() > 0) {
    if      (qs == "active")   { EvseProperties p(EvseState::Active);   LiteOverrideLimits l; override_apply(p, l); }
    else if (qs == "disabled") { EvseProperties p(EvseState::Disabled); LiteOverrideLimits l; override_apply(p, l); }
    else if (qs == "release" || qs == "clear") { override_clear(); }
    override_get_json(body);
  } else if (s_server.method() == HTTP_POST) {
    String b = s_server.arg("plain");
    EvseProperties props; LiteOverrideLimits lim;
    if (override_parse(b.c_str(), b.length(), props, lim)) {
      override_apply(props, lim); code = 201; body = "{\"msg\":\"Created\"}";
    } else { code = 400; body = "{\"msg\":\"Failed to parse JSON\"}"; }
  } else if (s_server.method() == HTTP_DELETE) {
    override_clear(); body = "{\"msg\":\"Deleted\"}";
  } else if (s_server.method() == HTTP_PATCH) {
    manual.toggle();
    s_ovrLimits = LiteOverrideLimits(); s_ovrExpired = false;
    EvseProperties tp; s_ovrEnabling = manual.getProperties(tp) && tp.getState() == EvseState::Active;
    body = "{\"msg\":\"Updated\"}";
  } else {
    override_get_json(body);   // GET
  }
  s_server.send(code, "application/json", body);
}
```

- [ ] **Step 4: Replace `sntp_ev_handler` + `handle_config`**

Delete `sntp_ev_handler` entirely. Replace `handle_config` (and add the `qarg`
helper just above it):

```cpp
// Query arg present and non-empty (mirrors the old mg_get_http_var "> 0" gate).
static bool qarg(const char *k, String &v) { v = s_server.arg(k); return v.length() > 0; }

static void handle_config() {
  String v;
  bool any = false;
  LiteEvseConfig cfg = s_cfg;
  if (qarg("max_current_hard", v)) { cfg.max_current_hard = v.toInt(); any = true; }
  if (qarg("max_current_soft", v)) { cfg.max_current_soft = v.toInt(); any = true; }

  if (qarg("tz_offset_min", v)) {
    LiteClockConfig cc; lite_config_load_clock(cc);
    cc.tz_offset_min = v.toInt(); lite_config_save_clock(cc);
    if (s_clock) s_clock->setTzOffsetMinutes(cc.tz_offset_min);
  }
  if (qarg("sntp_hostname", v)) {
    LiteClockConfig cc; lite_config_load_clock(cc);
    cc.sntp_hostname = v.c_str(); lite_config_save_clock(cc);
    s_sntpHost = v.c_str();
    // Task 3 adds: sntp_setservername(0, s_sntpHost.c_str());
  }

  LiteDivertConfig dcfg = s_divertCfg; bool dany = false;
  if (qarg("divert_enabled", v)) { dcfg.enabled = v.toInt() != 0; dany = true; }
  if (qarg("divert_type", v))    { dcfg.type = v.toInt() ? 1 : 0; dany = true; }
  if (qarg("divert_PV_ratio", v))              { dcfg.pv_ratio = v.toFloat(); dany = true; }
  if (qarg("divert_attack_smoothing_time", v)) { dcfg.attack_s = (uint32_t)v.toInt(); dany = true; }
  if (qarg("divert_decay_smoothing_time", v))  { dcfg.decay_s = (uint32_t)v.toInt(); dany = true; }
  if (qarg("divert_min_charge_time", v))       { dcfg.min_charge_s = (uint32_t)v.toInt(); dany = true; }
  if (dany) { lite_config_save_divert(dcfg); s_divertCfg = dcfg; }

  LiteShaperConfig scfg = s_shaperCfg; bool sany = false;
  if (qarg("current_shaper_enabled", v)) { scfg.enabled = v.toInt() != 0; sany = true; }
  if (qarg("current_shaper_max_pwr", v))          { scfg.max_pwr_w = (uint32_t)v.toInt(); sany = true; }
  if (qarg("current_shaper_smoothing_time", v))   { scfg.smoothing_s = (uint32_t)v.toInt(); sany = true; }
  if (qarg("current_shaper_data_maxinterval", v)) { scfg.data_maxinterval_s = (uint32_t)v.toInt(); sany = true; }
  if (qarg("current_shaper_min_pause_time", v))   { scfg.min_pause_s = (uint32_t)v.toInt(); sany = true; }
  if (sany) { lite_config_save_shaper(scfg); s_shaperCfg = scfg; }

  int status = 200;
  if (any) {
    cfg.max_current_hard = lite_clamp_service_max(cfg.max_current_hard);
    cfg.max_current_soft = lite_clamp_charge_current(cfg.max_current_soft, cfg.max_current_hard);
    bool saved = lite_config_save_evse(cfg);
    s_cfg = cfg;   // apply + cache even if persistence failed (best effort)
    if (s_mgr_ctrl) {
      s_mgr_ctrl->setTargetMaxCurrent((uint32_t)cfg.max_current_hard);
      s_mgr_ctrl->setTargetChargeCurrent((uint32_t)cfg.max_current_soft);
    }
    if (!saved) status = 503;   // applied but not persisted
  }

  String body; config_json(body);
  s_server.send(status, "application/json", body);
}
```

- [ ] **Step 5: Replace `ws_broadcast_status`/`handle_scan`/`handle_connect`/`ev_handler`**

Delete `ws_broadcast_status`. Replace `handle_scan`, `handle_connect`, and the
whole `ev_handler` with:

```cpp
static void handle_status() {
  if (s_server.method() == HTTP_POST) {
    String b = s_server.arg("plain");
    status_post_apply(b.c_str(), b.length());
    s_server.send(200, "application/json", "{\"msg\":\"OK\"}");
  } else {
    String body; build_status_json(body);
    s_server.send(200, "application/json", body);
  }
}

// GET /scan -> [{ssid,rssi,enc}]. Synchronous scan; scanDelete() after (UAF-safe).
static void handle_scan() {
  int16_t n = WiFi.scanNetworks();
  StaticJsonDocument<3072> doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < n && i < 32; i++) {
    String ssid = WiFi.SSID((uint8_t)i);
    if (ssid.length() == 0) continue;   // omit hidden
    JsonObject o = arr.createNestedObject();
    o["ssid"] = ssid;
    o["rssi"] = WiFi.RSSI((uint8_t)i);
    o["enc"]  = lite_provision_enc((int)WiFi.encryptionType((uint8_t)i));
  }
  WiFi.scanDelete();
  String body; serializeJson(doc, body);
  s_server.send(200, "application/json", body);
}

// GET /connect?ssid=&pass= -> save creds, 200, then deferred reboot. WebServer
// URL-decodes args (Parsing.cpp), so store directly — no second decode.
static void handle_connect() {
  String ssid = s_server.arg("ssid");
  String pass = s_server.arg("pass");
  if (ssid.length() == 0) {
    s_server.send(400, "application/json", "{\"msg\":\"ssid required\"}");
    return;
  }
  LiteWifiConfig c;
  c.ssid = ssid.c_str();
  if (pass.length() > 0) c.pass = pass.c_str();
  if (!lite_config_save_wifi(c)) {
    s_server.send(500, "application/json", "{\"msg\":\"save failed\"}");
    return;
  }
  s_server.send(200, "application/json", "{\"msg\":\"OK\"}");
  s_rebootPending = true;
  s_rebootAtMs    = millis() + 750;   // reboot from loop() after the response flushes
}

// GET / -> gzip bundle for the current mode. Binary-safe: sendContent writes raw
// bytes (gzip has NULs). Sockets stream it (bounded by TCP_SND_BUF) — no big mbuf.
static void handle_root() {
  const uint8_t *body = s_apMode ? SETUP_HTML_GZ : INDEX_HTML_GZ;
  unsigned len        = s_apMode ? SETUP_HTML_GZ_LEN : INDEX_HTML_GZ_LEN;
  s_server.sendHeader("Content-Encoding", "gzip");
  s_server.setContentLength(len);
  s_server.send(200, "text/html", "");
  s_server.sendContent((const char *)body, (size_t)len);
}

static void handle_not_found() {
  s_server.send(404, "text/plain", "not found");
}
```

- [ ] **Step 6: Rewrite `web_server_lite_begin` route registration**

Keep the existing config-load block (clock, schedule, EVSE cfg clamp+seed,
divert, shaper loads) **verbatim**. Replace the mongoose tail (`s_mgLock` create,
`mg_mgr_init`, `mg_bind`, `mg_set_protocol_http_websocket`) with:

```cpp
  s_server.on("/", handle_root);
  s_server.on("/status", handle_status);
  s_server.on("/config", handle_config);
  s_server.on("/override", handle_override);
  s_server.on("/schedule", handle_schedule);
  s_server.on(UriBraces("/schedule/{}"), HTTP_DELETE, handle_schedule_del_path);
  s_server.on("/scan", handle_scan);
  s_server.on("/connect", handle_connect);
  s_server.onNotFound(handle_not_found);
  s_server.begin();
  // Task 3 adds lwIP SNTP init here.
```

- [ ] **Step 7: Rewrite `web_server_lite_loop` transport bits**

Replace the leading `mg_mgr_poll(&s_mgr, 0);` with `s_server.handleClient();`.
Keep the deferred-reboot check and the override-limits / schedule / shaper /
divert control blocks **verbatim**. Delete the WS-push block, the mongoose SNTP
block, and the `mg_time()` idle-reap loop at the end (NTP is re-added in Task 3).
The loop now ends after the divert control block.

- [ ] **Step 8: Strip the mongoose build flags**

Edit `platformio.ini` `[env:openevse_lite]` `build_flags`: delete
`-D MG_ENABLE_SSL=0`, `-D MG_ENABLE_SNTP=1`, `-D CS_PLATFORM=CS_P_CUSTOM`, and
`-I lib/MongooseLite`. Keep everything else (`-D OPENEVSE_LITE`, `-D JUICEBOX_40`,
`-D LITE_EVSE_BACKEND_JUICEBOX`, `-D configTOTAL_HEAP_SIZE=262144`, `-I src/lite`,
`-D LT_LOGLEVEL=LT_LEVEL_NONE`, `-D interrupts=__enable_irq`,
`-D noInterrupts=__disable_irq`, `-lm`).

- [ ] **Step 9: Build for device**

Run: `pio run -e openevse_lite`
Expected: SUCCESS, flash lower than the mongoose build. If `arg("plain")` for the
POST body is empty, confirm against `cores/.../ext/WebServer/Parsing.cpp` (it
stores the body under key `plain`).

- [ ] **Step 10: Commit**

```bash
git add src/lite/web_server_lite.cpp platformio.ini
git commit -m "feat(lite): migrate web server MongooseLite -> LibreTiny WebServer

Transport swap onto thread-safe lwIP sockets (WebServer). Same endpoints;
WebSocket push removed (GUI polls /status); time sync temporarily off
(lwIP SNTP wired in a follow-up). Ends the mongoose NO_SYS=0 cross-thread
bug class. Pure handler logic unchanged."
```

---

## Task 2: Remove the WebSocket helper + mongoose build remnants

**Files:**
- Delete: `src/lite/lite_ws.h`, `src/lite/lite_ws.cpp`
- Modify: `platformio.ini` (`[env:native]` `build_src_filter`)

- [ ] **Step 1: Confirm `lite_ws` is now unreferenced**

Run: `grep -rn "lite_ws" src/ test/`
Expected: no matches. If any remain, resolve before deleting.

- [ ] **Step 2: Delete the files and the native build entry**

```bash
git rm src/lite/lite_ws.h src/lite/lite_ws.cpp
```
Edit `platformio.ini` `[env:native]` `build_src_filter`: remove the
`+<lite/lite_ws.cpp>` token.

- [ ] **Step 3: Build both environments**

Run: `pio run -e openevse_lite` → SUCCESS.
Run: `pio test -e native` → all suites PASS.

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "chore(lite): drop lite_ws WebSocket helper (push replaced by /status polling)"
```

---

## Task 3: Wire lwIP SNTP (requires the fork Prerequisite pulled)

**Files:** Modify `src/lite/web_server_lite.cpp`.

Do this only after the libretiny fork SNTP change is pushed and pulled
(`pio pkg update -e openevse_lite`).

- [ ] **Step 1: Pull the fork change**

Run: `pio pkg update -e openevse_lite`
Expected: the libretiny platform fast-forwards to the commit enabling SNTP.

- [ ] **Step 2: Add the SNTP include + the time-set hook**

In `web_server_lite.cpp`, after the other includes add:
```cpp
#include "lwip/apps/sntp.h"
```
Below the `s_server` static add the hook lwIP calls on each successful sync (it
runs on the tcpip thread, so just stash; the loop applies it):
```cpp
// lwIP SNTP calls this (SNTP_SET_SYSTEM_TIME in lwipopts.h) with Unix seconds.
// Stash only — applied to the clock from web_server_lite_loop() on the main task.
static volatile uint32_t s_sntpEpoch = 0;
static volatile bool     s_sntpHave  = false;
extern "C" void lite_sntp_set_system_time(unsigned int sec) {
  s_sntpEpoch = (uint32_t)sec;
  s_sntpHave  = true;
}
```

- [ ] **Step 3: Start SNTP in `begin()`**

After `s_server.begin();` add:
```cpp
  sntp_setoperatingmode(SNTP_OPMODE_POLL);
  sntp_setservername(0, s_sntpHost.c_str());   // s_sntpHost is a stable static String
  sntp_init();
```
And in `handle_config`'s `sntp_hostname` branch, after `s_sntpHost = v.c_str();`
add `sntp_setservername(0, s_sntpHost.c_str());` so a host change takes effect.

- [ ] **Step 4: Apply received time in `loop()`**

At the top of `web_server_lite_loop()` (after `s_server.handleClient();`) add:
```cpp
  if (s_sntpHave && s_clock) {
    s_sntpHave = false;
    s_clock->setEpoch((uint32_t)s_sntpEpoch, millis());
  }
```

- [ ] **Step 5: Build**

Run: `pio run -e openevse_lite`
Expected: SUCCESS (links `sntp_init`/`sntp_setservername` from the fork's lwIP).

- [ ] **Step 6: Commit**

```bash
git add src/lite/web_server_lite.cpp
git commit -m "feat(lite): time sync via lwIP SNTP (clock set from SNTP_SET_SYSTEM_TIME hook)"
```

---

## Task 4: Full verification + flash + hardware round-trip

**Files:** none (verification only).

- [ ] **Step 1: Native suite green** — `pio test -e native` → all PASS.

- [ ] **Step 2: Device build + flash-size note** — `pio run -e openevse_lite` →
  SUCCESS; record `Flash:`/`RAM:` (flash lower than mongoose; RAM ~69% with the
  256 KB heap).

- [ ] **Step 3: Flash the bench unit** — `pio run -e openevse_lite -t upload` →
  openocd two-image program + verify OK.

- [ ] **Step 4: SWD health check**
```bash
/usr/bin/openocd -c "adapter driver jlink" -c "transport select swd" -f target/efm32.cfg \
  -c "init" -c "halt" -c "mdw 0xE000ED08 1" -c "mdw 0xE000ED28 1" -c "mdw 0xE000ED2C 1" \
  -c "reg pc" -c "resume" -c "exit"
```
Expected: VTOR `0x00008000`, CFSR `0`, HFSR `0`, PC ≥ `0x8000`.

- [ ] **Step 5: Hardware round-trip (user bench)** — the gauntlet that failed
  under mongoose (needs a real associated client):
  1. Join `OpenEVSE-Lite-<id>` AP (WPA2 / `openevse`).
  2. `GET http://192.168.4.1/` — full setup page loads, no malloc-fail, no HardFault.
  3. Rescan several times while associated — list returns each time, no crash.
  4. `/connect` → saves → reboots into STA.
  5. Dashboard loads at the new IP and updates via `/status` polling.
  6. Power-cycle → creds persist (KVS); clock syncs via SNTP within a minute
     (network must allow outbound UDP/123).

- [ ] **Step 6: GUI coordination (hand to UI agent)** — drop the `/ws` WebSocket;
  poll `GET /status` (~1.5 s) in `WebSocket.svelte`; and the still-pending
  setup-AP-is-WPA2-`openevse` copy update.

---

## Notes / out of scope

- `lib/MongooseLite/` is dropped from the build but left on disk; physical
  deletion is a follow-up commit to keep this diff reviewable.
- WebSocket/SSE live push deferred (polling for now).
- Between Task 1 and Task 3 the clock is unsynced (schedule inert, `/status` omits
  `time`); this is intentional sequencing so the crash fix lands first.
- `WiFi.scanNetworks()` remains synchronous (~750 ms blocks `handleClient` during
  a scan) — acceptable on the setup page; async-scan is a future option.
