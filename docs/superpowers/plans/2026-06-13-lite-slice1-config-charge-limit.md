# Slice 1 — Config Foundation + Configurable Charge Limit + Service-Max Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the JuiceBox-lite keepalive charge current settable at runtime over HTTP, clamped to a configurable service-max ceiling, persisted to the FlashDB `kvs` partition, and restored on boot — replacing the hard-coded `_chargeLimit = 6`.

**Architecture:** Approach B (separate layers). A pure clamp-policy unit (native-tested) + a FlashDB-KVDB config store + a one-method control seam on the backend (`setChargeCurrent`) + `web_server_lite` orchestrating `GET`/`POST /config` and owning the boot load→clamp→apply. See spec `docs/superpowers/specs/2026-06-13-lite-slice1-config-charge-limit-design.md`.

**Tech Stack:** C++ (Arduino/LibreTiny EFM32GG11), FlashDB KVDB over FAL (`#include <flashdb.h>`, framework-vendored, partition `kvs` @ `FLASH_KVS_OFFSET 0x1F0000` / `FLASH_KVS_LENGTH 0x8000`, `FDB_WRITE_GRAN=32`), Mongoose 5.x raw C API, ArduinoJson 6.20, doctest (native env).

**Worktree:** `/home/rar/oevse/openevse-juicebox-lite` (branch `feature/juicebox-lite`). All paths below are relative to it.

---

## File Structure

- **Create** `src/lite/lite_charge_policy.h` / `.cpp` — pure clamp functions (no Arduino deps; compiled in both lite and native envs, unguarded).
- **Create** `test/test_lite_charge_policy/test_lite_charge_policy.cpp` — doctest for the clamp policy.
- **Modify** `src/lite/lite_evse_backend.h` — add `setChargeCurrent` / `getChargeCurrent` to the interface.
- **Modify** `src/lite/juicebox_backend.h` — implement the two seam methods inline.
- **Rewrite** `src/lite/lite_config_store.h` / `.cpp` — FlashDB KVDB backend; add `LiteEvseConfig` load/save.
- **Modify** `src/lite/web_server_lite.cpp` — RAM config cache, `GET`/`POST /config`, `/status` fields, boot load→clamp→apply in `web_server_lite_begin`.
- **Modify** `src/lite/main_lite.cpp` — call `lite_config_begin()` before `web_server_lite_begin`.
- **Modify** `platformio.ini` — re-include `lite_config_store.cpp` in `[env:openevse_lite]`; add `lite_charge_policy.cpp` to `[env:native]`.

---

### Task 1: Pure clamp policy (native-tested, TDD)

**Files:**
- Create: `src/lite/lite_charge_policy.h`
- Create: `src/lite/lite_charge_policy.cpp`
- Test: `test/test_lite_charge_policy/test_lite_charge_policy.cpp`
- Modify: `platformio.ini` (`[env:native]` `build_src_filter`)

- [ ] **Step 1: Write the failing test**

Create `test/test_lite_charge_policy/test_lite_charge_policy.cpp`:

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../src/lite/lite_charge_policy.h"

TEST_CASE("service-max clamps to [6, 48]") {
  CHECK(lite_clamp_service_max(32) == 32);   // in range, unchanged
  CHECK(lite_clamp_service_max(3)  == 6);    // below floor -> 6
  CHECK(lite_clamp_service_max(0)  == 6);    // zero -> 6
  CHECK(lite_clamp_service_max(80) == 48);   // above cap -> 48
  CHECK(lite_clamp_service_max(6)  == 6);    // boundary
  CHECK(lite_clamp_service_max(48) == 48);   // boundary
}

TEST_CASE("charge current clamps to [6, clamped service-max]") {
  CHECK(lite_clamp_charge_current(20, 32) == 20);  // soft within hard
  CHECK(lite_clamp_charge_current(40, 24) == 24);  // soft above hard -> hard
  CHECK(lite_clamp_charge_current(3,  32) == 6);   // soft below floor -> 6
  CHECK(lite_clamp_charge_current(40, 80) == 40);  // hard over-cap clamps to 48 first, 40 fits
  CHECK(lite_clamp_charge_current(60, 80) == 48);  // both over -> 48
  CHECK(lite_clamp_charge_current(20, 3)  == 6);   // hard below floor -> service-max 6 -> soft 6
}
```

- [ ] **Step 2: Register the test source in the native env**

In `platformio.ini`, edit the `[env:native]` `build_src_filter` line to append the new source:

```ini
build_src_filter = -<*> +<tsdb_sample.cpp> +<home_battery.cpp> +<lite/lite_random.cpp> +<lite/juicebox_proto.cpp> +<lite/lite_charge_policy.cpp>
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `pio test -e native -f test_lite_charge_policy`
Expected: FAIL to compile — `lite_charge_policy.h: No such file or directory`.

- [ ] **Step 4: Write the header**

Create `src/lite/lite_charge_policy.h`:

```cpp
#pragma once

// JuiceBox charge-current clamp policy. Pure integer functions (no Arduino
// deps) so they unit-test in the native doctest env. Shared by web_server_lite
// (HTTP set path + boot apply). Intentionally NOT guarded by OPENEVSE_LITE: the
// native test env does not define it and must still compile this unit.

static constexpr int JB_MIN_CURRENT = 6;   // J1772 pilot floor
static constexpr int JB_ABS_MAX     = 48;  // absolute safety cap regardless of config

// Clamp a service-max ceiling into [JB_MIN_CURRENT, JB_ABS_MAX].
int lite_clamp_service_max(int hard);

// Clamp a charge-current setpoint into [JB_MIN_CURRENT, lite_clamp_service_max(hard)].
int lite_clamp_charge_current(int soft, int hard);
```

- [ ] **Step 5: Write the implementation**

Create `src/lite/lite_charge_policy.cpp`:

```cpp
#include "lite_charge_policy.h"

static int clamp_int(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

int lite_clamp_service_max(int hard) {
  return clamp_int(hard, JB_MIN_CURRENT, JB_ABS_MAX);
}

int lite_clamp_charge_current(int soft, int hard) {
  return clamp_int(soft, JB_MIN_CURRENT, lite_clamp_service_max(hard));
}
```

- [ ] **Step 6: Run the test to verify it passes**

Run: `pio test -e native -f test_lite_charge_policy`
Expected: PASS — all CHECKs in both test cases pass.

- [ ] **Step 7: Commit**

```bash
git add src/lite/lite_charge_policy.h src/lite/lite_charge_policy.cpp test/test_lite_charge_policy/ platformio.ini
git commit -m "feat(lite): add pure charge-current clamp policy + native tests"
```

---

### Task 2: Control-seam write method on the backend

**Files:**
- Modify: `src/lite/lite_evse_backend.h`
- Modify: `src/lite/juicebox_backend.h`

No native test (the backend isn't compiled in the native env — it needs the FlashDB/LibreTiny build). Verified by the lite build compiling in Task 4's full build and by HW validation in Task 5. This task is a pure interface + inline-impl addition.

- [ ] **Step 1: Add the two methods to the interface**

In `src/lite/lite_evse_backend.h`, after the `getFault()` line (currently line 18, `virtual int getFault() const = 0;`), add:

```cpp
  // Control seam (write surface). Slice 1 ships only the single charge-current
  // setpoint; the full claim/priority model arrives in Slice 1.5.
  virtual void setChargeCurrent(int amps) = 0; // desired charge current (A); backend may clamp to its own floor
  virtual int  getChargeCurrent() const = 0;   // current advertised setpoint
```

- [ ] **Step 2: Implement them inline on JuiceBoxBackend**

In `src/lite/juicebox_backend.h`, after the `getFault()` inline (currently line 22, `int getFault() const override { return _status.fault; }`), add:

```cpp
  // Sets the keepalive's advertised charge current. The Atmel further clamps to
  // its 6 A floor / <81 A ceiling; host-side policy lives in lite_charge_policy.
  void setChargeCurrent(int amps) override { _chargeLimit = amps; }
  // Distinct from getAmps() (the Atmel's reported max/rating in $ES field A).
  int  getChargeCurrent() const override { return _chargeLimit; }
```

(Leave the existing `int _chargeLimit = 6;` member initializer untouched — it is the safety default if the boot apply is ever skipped.)

- [ ] **Step 3: Commit**

```bash
git add src/lite/lite_evse_backend.h src/lite/juicebox_backend.h
git commit -m "feat(lite): add setChargeCurrent/getChargeCurrent control seam"
```

---

### Task 3: FlashDB KVDB config store

**Files:**
- Rewrite: `src/lite/lite_config_store.h`
- Rewrite: `src/lite/lite_config_store.cpp`
- Modify: `platformio.ini` (`[env:openevse_lite]` `build_src_filter`)

No native test (FlashDB is device-only). Verified by the lite build in Task 4 + HW persistence in Task 5.

- [ ] **Step 1: Confirm no other lite file includes `<LittleFS.h>`**

Run: `grep -rn "LittleFS" src/lite/ --include=*.cpp --include=*.h`
Expected: matches only in `lite_config_store.cpp`, `lite_config_store.h` (none), and the shim `src/lite/LittleFS.h`. If any *other* lite TU includes `<LittleFS.h>`, stop and report — the `-I src/lite` shim removal assumption is wrong. (We are NOT removing the `-I src/lite` flag in this slice; this check just confirms the store is the only LittleFS consumer so the rewrite is self-contained.)

- [ ] **Step 2: Rewrite the header**

Replace the entire contents of `src/lite/lite_config_store.h` with:

```cpp
#pragma once
#ifdef OPENEVSE_LITE
#include <Arduino.h>

struct LiteWifiConfig { String ssid; String pass; };

// Typed EVSE config. Key names mirror upstream app_config so later module lifts
// find what they expect. Each field persists as its own FlashDB KV blob.
struct LiteEvseConfig {
  int max_current_soft; // active charge-current setpoint (A) the keepalive advertises
  int max_current_hard; // service-max ceiling (A) — install rating; soft is clamped to this
};

// Contract: call lite_config_begin() once at boot before any load/save/erase.
// Backed by a FlashDB KVDB on the `kvs` FAL partition (0x1F0000+0x8000).
bool lite_config_begin();                         // fdb_kvdb_init on the kvs partition; true on success

bool lite_config_load_wifi(LiteWifiConfig &out);  // false if no ssid stored yet
bool lite_config_save_wifi(const LiteWifiConfig &in);

bool lite_config_load_evse(LiteEvseConfig &out);  // false if max_current_hard key absent (use defaults)
bool lite_config_save_evse(const LiteEvseConfig &in);

void lite_config_erase();                         // wipe WiFi creds (eraseConfig)
#endif
```

- [ ] **Step 3: Rewrite the implementation**

Replace the entire contents of `src/lite/lite_config_store.cpp` with:

```cpp
#ifdef OPENEVSE_LITE
#include "lite_config_store.h"

#include <flashdb.h>
#include <string.h>

// FlashDB KVDB config store on the `kvs` FAL partition (FLASH_KVS_OFFSET
// 0x1F0000 + 0x8000). FlashDB is framework-vendored (libflashdb.a); the EFM32
// build sets FDB_WRITE_GRAN=32 (word-only flash) so KVs survive reboot — proven
// on the bench. LibreTiny ships only the IPreferences interface (no concrete
// Preferences class), so FlashDB is the real KV API here.

static struct fdb_kvdb s_kvdb;
static bool            s_ready = false;

bool lite_config_begin()
{
  // "env" = db name, "kvs" = FAL partition name, NULL lock (single-threaded loop()).
  fdb_err_t err = fdb_kvdb_init(&s_kvdb, "env", "kvs", NULL, NULL);
  s_ready = (err == FDB_NO_ERR);
  return s_ready;
}

// --- string helpers (WiFi creds) ---

static bool kv_get_str(const char *key, String &out)
{
  if (!s_ready) return false;
  char buf[64] = {0};
  struct fdb_blob blob;
  fdb_kv_get_blob(&s_kvdb, key, fdb_blob_make(&blob, buf, sizeof(buf) - 1));
  if (blob.saved.len == 0) return false; // key absent
  buf[sizeof(buf) - 1] = '\0';
  out = buf;
  return true;
}

static bool kv_set_str(const char *key, const String &val)
{
  if (!s_ready) return false;
  struct fdb_blob blob;
  // +1 to persist the trailing NUL so reads are always terminated.
  fdb_err_t err = fdb_kv_set_blob(
      &s_kvdb, key, fdb_blob_make(&blob, val.c_str(), val.length() + 1));
  return err == FDB_NO_ERR;
}

// --- int helpers (EVSE config) ---

static bool kv_get_int(const char *key, int &out)
{
  if (!s_ready) return false;
  int v = 0;
  struct fdb_blob blob;
  fdb_kv_get_blob(&s_kvdb, key, fdb_blob_make(&blob, &v, sizeof(v)));
  if (blob.saved.len == 0) return false; // key absent
  out = v;
  return true;
}

static bool kv_set_int(const char *key, int v)
{
  if (!s_ready) return false;
  struct fdb_blob blob;
  fdb_err_t err = fdb_kv_set_blob(&s_kvdb, key, fdb_blob_make(&blob, &v, sizeof(v)));
  return err == FDB_NO_ERR;
}

// --- WiFi creds ---

bool lite_config_load_wifi(LiteWifiConfig &out)
{
  String ssid;
  if (!kv_get_str("wifi_ssid", ssid) || ssid.length() == 0) {
    return false; // no/blank ssid -> fall back to build-flag defaults
  }
  out.ssid = ssid;
  String pass;
  out.pass = kv_get_str("wifi_pass", pass) ? pass : String("");
  return true;
}

bool lite_config_save_wifi(const LiteWifiConfig &in)
{
  return kv_set_str("wifi_ssid", in.ssid) && kv_set_str("wifi_pass", in.pass);
}

// --- EVSE config ---

bool lite_config_load_evse(LiteEvseConfig &out)
{
  int hard = 0;
  if (!kv_get_int("max_current_hard", hard)) {
    return false; // unset store -> caller applies defaults
  }
  out.max_current_hard = hard;
  int soft = hard;
  out.max_current_soft = kv_get_int("max_current_soft", soft) ? soft : hard;
  return true;
}

bool lite_config_save_evse(const LiteEvseConfig &in)
{
  return kv_set_int("max_current_hard", in.max_current_hard) &&
         kv_set_int("max_current_soft", in.max_current_soft);
}

void lite_config_erase()
{
  if (!s_ready) return;
  fdb_kv_del(&s_kvdb, "wifi_ssid");
  fdb_kv_del(&s_kvdb, "wifi_pass");
}
#endif
```

- [ ] **Step 4: Re-include the store in the lite build**

In `platformio.ini`, in `[env:openevse_lite]` `build_src_filter`, remove the exclusion line `-<lite/lite_config_store.cpp>` so the store compiles. The block should read:

```ini
build_src_filter =
  -<*>
  +<lite/>
  -<lite/spike_main.cpp>
  -<lite/lite_evse_stubs.cpp>
  -<lite/debug_lite.cpp>
```

- [ ] **Step 5: Build the lite firmware**

Run: `pio run -e openevse_lite`
Expected: SUCCESS — links `libflashdb.a`; no `LittleFS` symbols remain in the store.
(If `<flashdb.h>` is not found, the fork checkout in `/home/rar/dev/libretiny` is not on `feature/silabs-efm32gg11-ota` — stop and report.)

- [ ] **Step 6: Commit**

```bash
git add src/lite/lite_config_store.h src/lite/lite_config_store.cpp platformio.ini
git commit -m "feat(lite): back config store with FlashDB KVDB on the kvs partition"
```

---

### Task 4: Web endpoint + boot flow

**Files:**
- Modify: `src/lite/web_server_lite.cpp`
- Modify: `src/lite/main_lite.cpp`

- [ ] **Step 1: Add includes + RAM config cache + clamp helper to web_server_lite.cpp**

In `src/lite/web_server_lite.cpp`, after the existing `#include "espal_lite.h"` (line 14), add:

```cpp
#include "lite_config_store.h"
#include "lite_charge_policy.h"
```

Then, after the `static LiteEvseBackend *s_backend = NULL;` line (line 23), add the RAM cache:

```cpp
// Active EVSE config cached in RAM so /status and GET /config never touch flash.
// Seeded at web_server_lite_begin() from the store (or defaults) and updated on POST.
static LiteEvseConfig s_cfg = { 32, 32 }; // {soft, hard} defaults (smallest JuiceBox sold)
```

- [ ] **Step 2: Emit the config fields in /status (and enlarge the doc)**

In `build_status_json`, inside the `if (s_backend)` block, after `s_backend->addStatusFields(doc);` (line 36), add:

```cpp
    doc["max_current_soft"] = s_cfg.max_current_soft;
    doc["max_current_hard"] = s_cfg.max_current_hard;
```

Then bump the document capacity in the same function: change the declaration `StaticJsonDocument<256> doc;` (line 28) to `StaticJsonDocument<512> doc;`. Rationale: `addStatusFields` can already populate `md` (≤48), `wr` (≤48), `wc` (≤24), `hw` (≤24), `fw` (≤16) plus the base fields, which approaches/exceeds 256; the two new fields would push a worst-case payload over capacity and silently truncate the JSON. 512 bytes of stack is safe here.

- [ ] **Step 3: Add the /config GET + POST handlers**

In `web_server_lite.cpp`, immediately above `static void ev_handler(...)` (line 45), add two helpers:

```cpp
// Serialize the cached config as the canonical /config response body.
static void config_json(String &out)
{
  StaticJsonDocument<64> doc;
  doc["max_current_soft"] = s_cfg.max_current_soft;
  doc["max_current_hard"] = s_cfg.max_current_hard;
  serializeJson(doc, out);
}

// POST /config?max_current_soft=N&max_current_hard=M (either or both).
static void handle_config_post(struct mg_connection *nc, struct http_message *hm)
{
  char val[8];
  bool any = false;
  LiteEvseConfig cfg = s_cfg; // start from current, allow partial update

  if (mg_get_http_var(&hm->query_string, "max_current_hard", val, sizeof(val)) > 0) {
    cfg.max_current_hard = atoi(val);
    any = true;
  }
  if (mg_get_http_var(&hm->query_string, "max_current_soft", val, sizeof(val)) > 0) {
    cfg.max_current_soft = atoi(val);
    any = true;
  }

  if (!any) {
    const char *body = "no params";
    mg_send_head(nc, 400, strlen(body), "Content-Type: text/plain");
    mg_printf(nc, "%s", body);
    return;
  }

  cfg.max_current_hard = lite_clamp_service_max(cfg.max_current_hard);
  cfg.max_current_soft = lite_clamp_charge_current(cfg.max_current_soft, cfg.max_current_hard);

  bool saved = lite_config_save_evse(cfg);

  // Apply + cache even if persistence failed (best effort).
  s_cfg = cfg;
  if (s_backend) {
    s_backend->setChargeCurrent(cfg.max_current_soft);
  }

  String body;
  config_json(body);
  // 503 signals "applied but not persisted" so the caller knows it won't survive reboot.
  mg_send_head(nc, saved ? 200 : 503, body.length(), "Content-Type: application/json");
  mg_printf(nc, "%s", body.c_str());
}
```

- [ ] **Step 4: Dispatch /config in ev_handler**

In `ev_handler`, add a branch before the `} else if (mg_vcmp(&hm->uri, "/") == 0) {` line (line 59):

```cpp
  } else if (mg_vcmp(&hm->uri, "/config") == 0) {
    if (mg_vcmp(&hm->method, "POST") == 0) {
      handle_config_post(nc, hm);
    } else {
      String body;
      config_json(body);
      mg_send_head(nc, 200, body.length(), "Content-Type: application/json");
      mg_printf(nc, "%s", body.c_str());
    }
```

- [ ] **Step 5: Boot load→clamp→apply in web_server_lite_begin**

In `web_server_lite_begin`, after `s_backend = &backend;` (line 74), add:

```cpp
  // Load persisted config (or keep the 32/32 defaults), clamp, apply to the backend.
  if (!lite_config_load_evse(s_cfg)) {
    s_cfg = (LiteEvseConfig){ 32, 32 };
  }
  s_cfg.max_current_hard = lite_clamp_service_max(s_cfg.max_current_hard);
  s_cfg.max_current_soft = lite_clamp_charge_current(s_cfg.max_current_soft, s_cfg.max_current_hard);
  backend.setChargeCurrent(s_cfg.max_current_soft);
```

- [ ] **Step 6: Init the KVDB before web_server_lite_begin in main_lite.cpp**

In `src/lite/main_lite.cpp`, in `setup()`, replace the line `s_backend.begin();` and the following `web_server_lite_begin(s_backend);` (lines 50-51) with:

```cpp
  s_backend.begin();
  lite_config_begin();              // mount FlashDB KVDB (kvs partition) before config load
  web_server_lite_begin(s_backend); // loads config -> clamps -> applies charge current
```

Add the store include near the other lite includes (after `#include "lite_evse_backend.h"`, line 10):

```cpp
#include "lite_config_store.h"
```

- [ ] **Step 7: Build the lite firmware**

Run: `pio run -e openevse_lite`
Expected: SUCCESS. Note the flash/RAM percentages from the output for Task 5.

- [ ] **Step 8: Commit**

```bash
git add src/lite/web_server_lite.cpp src/lite/main_lite.cpp
git commit -m "feat(lite): GET/POST /config charge limit + service max with boot apply"
```

---

### Task 5: Full build, flash/RAM measurement, HW validation

**Files:** none (verification only).

- [ ] **Step 1: Clean build + record size**

Run: `pio run -e openevse_lite 2>&1 | tail -20`
Expected: SUCCESS. Record the `RAM:` and `Flash:` lines (the roadmap requires per-slice measurement; the bank is now 960 KiB after the OTA partition resize). Confirm Flash is well under 960 KiB.

- [ ] **Step 2: Run the full native test suite (no regressions)**

Run: `pio test -e native`
Expected: PASS — `test_lite_charge_policy` plus the existing `test_juicebox_proto`, `test_lite_random`, `test_espal_lite`, `test_home_battery`, `test_tsdb_sample`, `test_native_smoke` all pass.

- [ ] **Step 3: Hand off HW validation to the user**

The build/flash uses `scripts/lite_flash.sh` (two-image: `bootloader.bin@0x0` + `firmware_a.bin@0x8000`). Because the OTA banks moved (992→960 KiB), a **full re-flash** is required, and the first boot should erase the kvs+meta region for a clean KVDB:

```
flash erase_address 0x1F0000 0xA000   # clears kvs + metadata
```

Provide the user this validation checklist (they run it on the bench):
1. Flash, boot. `curl http://<ip>/status` → shows `max_current_soft:32, max_current_hard:32` (defaults), keepalive on VCOM shows `$SL002:32`.
2. `curl -X POST 'http://<ip>/config?max_current_hard=24&max_current_soft=20'` → response `{"max_current_soft":20,"max_current_hard":24}`; `/status` shows 20/24; VCOM keepalive shows `$SL002:20`.
3. Power-cycle → `/status` still 20/24, keepalive still `$SL002:20` (persistence across reboot).
4. `curl -X POST 'http://<ip>/config?max_current_soft=40'` (hard still 24) → response + `/status` clamp soft to 24; keepalive `$SL002:24`.

- [ ] **Step 4: Mark task #105 complete after the user confirms HW validation**

Do not push unless the user asks. After user confirmation, the slice is done.

---

## Notes for the executor
- **Do not push or flash** unless the user explicitly asks. HW validation is the user's bench step.
- The fork checkout `/home/rar/dev/libretiny` must be on `feature/silabs-efm32gg11-ota` (HEAD `a526fff`) for `<flashdb.h>` and the `kvs` partition to exist. It already is.
- Never `program raw_firmware.elf` — use `scripts/lite_flash.sh` (bricks the bootloader otherwise).
- WiFi creds stay on build flags this slice; the store's WiFi accessors are rewritten but unused by `main_lite`.
