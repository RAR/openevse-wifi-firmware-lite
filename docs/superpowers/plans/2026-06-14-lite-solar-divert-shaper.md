# JuiceBox-lite Solar-Divert + Load-Shaping + 3c Push-In — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fed solar/grid/voltage/site-power over `POST /status`, the lite firmware autonomously follows excess solar (divert) and caps total current to a site-power budget (shaper), driving the ATmega via the existing `$AL` keepalive — parity with standard OpenEVSE.

**Architecture:** Three native-tested pure units (`lite_input_filter`, `lite_feed`, `lite_divert`, `lite_shaper`) transcribed verbatim from upstream `InputFilter`/`divert.cpp`/`current_shaper.cpp`, plus thin glue in `web_server_lite.cpp` (a `POST /status` receiver + per-loop claims through the existing `LiteEvseManager` seam). Divert claims `Active@Divert(50)` / `Disabled@Default(10)`; shaper claims `MaxCurrent@Safety(5000)`; `lite_evse_arbitrate` resolves and `apply()` clamps to the 6 A floor / hardware.

**Tech Stack:** C++17, doctest (native `[env:native]`), ArduinoJson 6.20.1, Mongoose 6 (MongooseLite), FlashDB KVDB, PlatformIO/LibreTiny (EFM32GG11).

**Reference:** `docs/superpowers/specs/2026-06-14-lite-solar-divert-shaper-design.md`

---

## File Structure

- **Create** `src/lite/lite_input_filter.{h,cpp}` — pure EMA smoothing (transcribes `InputFilter`).
- **Create** `src/lite/lite_feed.{h,cpp}` — pushed-feed store (solar/grid_ie/voltage/shaper_live_pwr + timestamps) + pure freshness predicate.
- **Create** `src/lite/lite_divert.{h,cpp}` — pure divert decision (transcribes `DivertTask::update_state` Eco path).
- **Create** `src/lite/lite_shaper.{h,cpp}` — pure shaper cap (transcribes `CurrentShaperTask::shapeCurrent`).
- **Create** `test/test_lite_input_filter/`, `test/test_lite_feed/`, `test/test_lite_divert/`, `test/test_lite_shaper/` — doctest suites.
- **Modify** `src/lite/lite_config_store.{h,cpp}` — `LiteDivertConfig`/`LiteShaperConfig` structs + blob load/save.
- **Modify** `src/lite/web_server_lite.cpp` — `POST /status` handler, `/status` echo fields, `/config` divert/shaper params, per-loop divert + shaper glue.
- **Modify** `platformio.ini` — add the four new `.cpp` to `[env:native]` `build_src_filter`.

Conventions (match existing lite units): pure headers are `#pragma once` + `<stdint.h>`, **no `OPENEVSE_LITE` guard**; test files start `#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN` / `#include "doctest.h"` / `#include "../../src/lite/<unit>.h"`. Commit `user.name='Andrew Rankin' user.email='andrewrankin@gmail.com'`; **no Co-Authored-By**.

---

# Sub-slice 1 — Feed receiver (3c) + smoothing filter

### Task 1: `lite_input_filter` pure EMA (TDD, native)

**Files:** Create `src/lite/lite_input_filter.{h,cpp}`, `test/test_lite_input_filter/test_lite_input_filter.cpp`; Modify `platformio.ini`.

- [ ] **Step 1: Header** — `src/lite/lite_input_filter.h`:

```cpp
#pragma once
#include <stdint.h>

// Exponential smoothing toward `input`, transcribed from standard-fw InputFilter:
//   factor = (tau_s>0) ? 1 - exp(-delta_s / max(tau_s, LITE_FILTER_MIN_TAU)) : 1.0
//   result = filtered + factor * (input - filtered)
// tau_s/delta_s in seconds. tau_s==0 disables filtering (returns input). Pure — no millis().
double lite_input_filter(double input, double filtered, uint32_t tau_s, uint32_t delta_s);
```

- [ ] **Step 2: Failing test** — `test/test_lite_input_filter/test_lite_input_filter.cpp`:

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../src/lite/lite_input_filter.h"

TEST_CASE("tau 0 disables filtering") {
  CHECK(lite_input_filter(100.0, 0.0, 0, 5) == doctest::Approx(100.0));
}
TEST_CASE("moves toward input by the decay factor") {
  // tau=10, delta=10 -> factor = 1 - e^-1 = 0.6321; from 0 toward 100 -> ~63.21
  CHECK(lite_input_filter(100.0, 0.0, 10, 10) == doctest::Approx(63.212).epsilon(0.001));
}
TEST_CASE("tau below MIN_TAU is clamped to MIN_TAU(10)") {
  // tau=2 clamps to 10, so same as the tau=10 case above
  CHECK(lite_input_filter(100.0, 0.0, 2, 10) == doctest::Approx(63.212).epsilon(0.001));
}
TEST_CASE("large delta approaches input") {
  CHECK(lite_input_filter(50.0, 0.0, 10, 1000) == doctest::Approx(50.0).epsilon(1e-6));
}
```

- [ ] **Step 3: Run, verify fail** — `pio test -e native -f test_lite_input_filter` → FAIL (undefined ref).

- [ ] **Step 4: Implement** — `src/lite/lite_input_filter.cpp`:

```cpp
#include "lite_input_filter.h"
#include <math.h>

#ifndef LITE_FILTER_MIN_TAU
#define LITE_FILTER_MIN_TAU 10u   // minimum tau (s), matches upstream INPUT_FILTER_MIN_TAU
#endif

double lite_input_filter(double input, double filtered, uint32_t tau_s, uint32_t delta_s)
{
  double factor;
  if (tau_s > 0) {
    if (tau_s < LITE_FILTER_MIN_TAU) tau_s = LITE_FILTER_MIN_TAU;
    factor = 1.0 - exp(-1.0 * ((double)delta_s / (double)tau_s));
  } else {
    factor = 1.0;   // tau 0 => no filtering
  }
  return filtered + factor * (input - filtered);
}
```

- [ ] **Step 5: Native filter** — in `platformio.ini`, append to the `[env:native]` `build_src_filter` line: ` +<lite/lite_input_filter.cpp>`

- [ ] **Step 6: Run, verify pass** — `pio test -e native -f test_lite_input_filter` → PASS.

- [ ] **Step 7: Commit**

```bash
git add src/lite/lite_input_filter.h src/lite/lite_input_filter.cpp test/test_lite_input_filter/ platformio.ini
git commit -m "feat(lite): pure exponential smoothing filter (native-tested)"
```

---

### Task 2: `lite_feed` pushed-feed store + freshness (TDD, native)

**Files:** Create `src/lite/lite_feed.{h,cpp}`, `test/test_lite_feed/test_lite_feed.cpp`; Modify `platformio.ini`.

- [ ] **Step 1: Header** — `src/lite/lite_feed.h`:

```cpp
#pragma once
#include <stdint.h>

// Store for sensor values pushed in via POST /status (3c). Each value carries the
// millis() timestamp of its last update + a validity flag (false until first set).
// Setters take now_ms explicitly so the unit is pure / native-testable.
struct LiteFeed {
  int      solar_w   = 0; uint32_t solar_ms   = 0; bool solar_valid   = false;
  int      grid_ie_w = 0; uint32_t grid_ms    = 0; bool grid_valid    = false;
  double   voltage   = 0; uint32_t voltage_ms = 0; bool voltage_valid = false;
  int      shaper_w  = 0; uint32_t shaper_ms  = 0; bool shaper_valid  = false;
};

void lite_feed_set_solar  (LiteFeed&, int w,      uint32_t now_ms);
void lite_feed_set_grid_ie(LiteFeed&, int w,      uint32_t now_ms);
void lite_feed_set_voltage(LiteFeed&, double v,   uint32_t now_ms);
void lite_feed_set_shaper (LiteFeed&, int w,      uint32_t now_ms);

// True if `last_ms` was updated within max_age_ms of now_ms. Unsigned subtraction is
// wrap-safe across the 32-bit millis() rollover. `valid` must also be true.
bool lite_feed_fresh(bool valid, uint32_t last_ms, uint32_t now_ms, uint32_t max_age_ms);
```

- [ ] **Step 2: Failing test** — `test/test_lite_feed/test_lite_feed.cpp`:

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../src/lite/lite_feed.h"

TEST_CASE("setters stamp value + time + validity") {
  LiteFeed f;
  CHECK_FALSE(f.solar_valid);
  lite_feed_set_solar(f, 1500, 12345);
  CHECK(f.solar_w == 1500); CHECK(f.solar_ms == 12345); CHECK(f.solar_valid);
  lite_feed_set_voltage(f, 241.5, 50);
  CHECK(f.voltage == doctest::Approx(241.5)); CHECK(f.voltage_valid);
}
TEST_CASE("freshness boundary + invalid") {
  CHECK_FALSE(lite_feed_fresh(false, 0, 0, 1000));      // never set -> stale
  CHECK(lite_feed_fresh(true, 0, 1000, 1000));          // exactly max age -> fresh (inclusive)
  CHECK_FALSE(lite_feed_fresh(true, 0, 1001, 1000));    // just past -> stale
  CHECK(lite_feed_fresh(true, 0, 999, 1000));           // within -> fresh
}
TEST_CASE("freshness wrap-safe across millis rollover") {
  uint32_t last = 0xFFFFFF00u, now = 0x00000064u;       // delta = 356
  CHECK(lite_feed_fresh(true, last, now, 500));         // 356 <= 500 -> fresh
  CHECK_FALSE(lite_feed_fresh(true, last, now, 200));   // 356 > 200 -> stale
}
```

- [ ] **Step 3: Run, verify fail** — `pio test -e native -f test_lite_feed` → FAIL.

- [ ] **Step 4: Implement** — `src/lite/lite_feed.cpp`:

```cpp
#include "lite_feed.h"

void lite_feed_set_solar  (LiteFeed &f, int w,    uint32_t now_ms) { f.solar_w = w;   f.solar_ms = now_ms;   f.solar_valid = true; }
void lite_feed_set_grid_ie(LiteFeed &f, int w,    uint32_t now_ms) { f.grid_ie_w = w; f.grid_ms = now_ms;    f.grid_valid = true; }
void lite_feed_set_voltage(LiteFeed &f, double v, uint32_t now_ms) { f.voltage = v;   f.voltage_ms = now_ms; f.voltage_valid = true; }
void lite_feed_set_shaper (LiteFeed &f, int w,    uint32_t now_ms) { f.shaper_w = w;  f.shaper_ms = now_ms;  f.shaper_valid = true; }

bool lite_feed_fresh(bool valid, uint32_t last_ms, uint32_t now_ms, uint32_t max_age_ms)
{
  return valid && ((uint32_t)(now_ms - last_ms) <= max_age_ms);
}
```

- [ ] **Step 5: Native filter** — append ` +<lite/lite_feed.cpp>` to `[env:native]` `build_src_filter`.

- [ ] **Step 6: Run, verify pass** — `pio test -e native -f test_lite_feed` → PASS.

- [ ] **Step 7: Commit**

```bash
git add src/lite/lite_feed.h src/lite/lite_feed.cpp test/test_lite_feed/ platformio.ini
git commit -m "feat(lite): pushed-feed store + wrap-safe freshness (native-tested)"
```

---

### Task 3: `POST /status` receiver + `/status` echo (glue, device build)

**Files:** Modify `src/lite/web_server_lite.cpp`.

- [ ] **Step 1: Include + feed instance** — after the other `#include "lite_*.h"` lines near the top of `web_server_lite.cpp`, add `#include "lite_feed.h"`. Next to the other file-static state (near `static LiteEvseConfig s_cfg`), add:

```cpp
// Pushed sensor feed (3c POST /status). Read by the divert/shaper glue below.
static LiteFeed s_feed;
```

- [ ] **Step 2: POST /status parser** — add a helper immediately above `build_status_json`:

```cpp
// Parse a POST /status JSON body into the pushed-feed store. Each key is optional
// (omit-when-absent): only present keys update. Mirrors the OpenEVSE handleStatusPost
// contract the firstof9 integration already sends.
static void status_post_apply(const char *body, size_t len)
{
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, body, len) != DeserializationError::Ok) return;
  uint32_t now = millis();
  if (doc.containsKey("solar"))           lite_feed_set_solar  (s_feed, doc["solar"].as<int>(),           now);
  if (doc.containsKey("grid_ie"))         lite_feed_set_grid_ie(s_feed, doc["grid_ie"].as<int>(),         now);
  if (doc.containsKey("voltage"))         lite_feed_set_voltage(s_feed, doc["voltage"].as<double>(),      now);
  if (doc.containsKey("shaper_live_pwr")) lite_feed_set_shaper (s_feed, doc["shaper_live_pwr"].as<int>(), now);
}
```

- [ ] **Step 3: Route the POST** — in `ev_handler`, the `/status` branch currently always builds the GET body (around `if (mg_vcmp(&hm->uri, "/status") == 0)`). Replace that branch with:

```cpp
  if (mg_vcmp(&hm->uri, "/status") == 0) {
    if (mg_vcmp(&hm->method, "POST") == 0) {
      status_post_apply(hm->body.p, hm->body.len);
      const char *ok = "{\"msg\":\"OK\"}";
      mg_send_head(nc, 200, strlen(ok), "Content-Type: application/json");
      mg_printf(nc, "%s", ok);
    } else {
      String body;
      build_status_json(body);
      mg_send_head(nc, 200, body.length(), "Content-Type: application/json");
      mg_printf(nc, "%s", body.c_str());
    }
  } else if (...) // existing /config, /override, etc. unchanged
```

(Match the exact send idiom already used in that branch; keep the surrounding `else if` chain.)

- [ ] **Step 4: Echo stored feed in GET /status** — in `build_status_json`, just before `serializeJson(doc, out);`, add (omit-when-absent — only emit valid values):

```cpp
  // Echo the freshest pushed feed (3c) so dashboards / the HA integration can read it back.
  if (s_feed.solar_valid)   doc["solar"]           = s_feed.solar_w;
  if (s_feed.grid_valid)    doc["grid_ie"]         = s_feed.grid_ie_w;
  if (s_feed.voltage_valid) doc["voltage"]         = s_feed.voltage;
  if (s_feed.shaper_valid)  doc["shaper_live_pwr"] = s_feed.shaper_w;
```

If `build_status_json` uses a sized `StaticJsonDocument`, bump its capacity to cover four more keys (e.g. +64 bytes); verify `out` isn't truncated after the build.

- [ ] **Step 5: Build device env** — `pio run -e openevse_lite` → SUCCESS. Record flash %.

- [ ] **Step 6: Commit**

```bash
git add src/lite/web_server_lite.cpp
git commit -m "feat(lite): POST /status feed receiver (3c) + echo pushed solar/grid/voltage/shaper"
```

---

# Sub-slice 2 — Solar divert

### Task 4: `lite_divert` pure decision (TDD, native)

**Files:** Create `src/lite/lite_divert.{h,cpp}`, `test/test_lite_divert/test_lite_divert.cpp`; Modify `platformio.ini`.

- [ ] **Step 1: Header** — `src/lite/lite_divert.h`:

```cpp
#pragma once
#include <stdint.h>

#ifndef LITE_DIVERT_HYSTERESIS
#define LITE_DIVERT_HYSTERESIS 0.5   // A, matches upstream EVSE_DIVERT_HYSTERESIS
#endif

enum class LiteDivertType  : uint8_t { Solar = 0, Grid = 1 };
enum class LiteDivertAction : uint8_t { Charge, Stop, Hold };  // Hold = no claim change

struct LiteDivertCfg {
  LiteDivertType type;
  double   pv_ratio;        // divert_PV_ratio (1.1)
  uint32_t attack_s;        // divert_attack_smoothing_time (20)
  uint32_t decay_s;         // divert_decay_smoothing_time (600)
  int      min_current_a;   // J1772 floor (6)
};
struct LiteDivertState { double smoothed_available; };  // carried across calls

struct LiteDivertResult {
  LiteDivertAction action;
  int    charge_rate_a;
  double available;
  double smoothed;
};

// One tick of the Eco-mode divert decision (transcribed from DivertTask::update_state).
// evse_present_a = the EVSE's own current contribution (lite passes the last commanded
// charge current — JuiceBox has no reliable live-draw readback). currently_active = does
// divert hold an Active claim now. min_charge_elapsed = has divert_min_charge_time passed
// since charging began. delta_s = seconds since the previous call (for smoothing).
LiteDivertResult lite_divert_eval(const LiteDivertCfg &cfg, LiteDivertState &st,
    int solar_w, int grid_ie_w, double voltage, int evse_present_a,
    bool currently_active, bool min_charge_elapsed, uint32_t delta_s);
```

- [ ] **Step 2: Failing test** — `test/test_lite_divert/test_lite_divert.cpp`:

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../src/lite/lite_divert.h"

static LiteDivertCfg solar_cfg() { return { LiteDivertType::Solar, 1.1, 20, 600, 6 }; }

TEST_CASE("SOLAR available = solar / voltage; charges above trigger") {
  LiteDivertCfg c = solar_cfg();
  LiteDivertState st{0};
  // 2400 W / 240 V = 10 A. tau=20 (rising, clamps fine), big delta -> smoothed ~= 10.
  LiteDivertResult r = lite_divert_eval(c, st, 2400, 0, 240.0, 0, false, false, 100000);
  CHECK(r.available == doctest::Approx(10.0));
  CHECK(r.charge_rate_a == 10);
  CHECK(r.action == LiteDivertAction::Charge);   // 10 >= trigger(6*1.0)+0.5
}
TEST_CASE("SOLAR below trigger holds (not charging) and stops only after min-charge") {
  LiteDivertCfg c = solar_cfg();
  LiteDivertState st{0};
  // 600 W / 240 V = 2.5 A, below trigger 6.0 -> not charging yet => Hold
  LiteDivertResult r = lite_divert_eval(c, st, 600, 0, 240.0, 0, false, false, 100000);
  CHECK(r.action == LiteDivertAction::Hold);
  // Now active but min-charge NOT elapsed -> still Hold (relay/car protection)
  st.smoothed_available = 0;
  r = lite_divert_eval(c, st, 600, 0, 240.0, 0, /*active*/true, /*elapsed*/false, 100000);
  CHECK(r.action == LiteDivertAction::Hold);
  // Active AND min-charge elapsed -> Stop
  st.smoothed_available = 0;
  r = lite_divert_eval(c, st, 600, 0, 240.0, 0, true, true, 100000);
  CHECK(r.action == LiteDivertAction::Stop);
}
TEST_CASE("charge-rate rounds up only when fractional part exceeds min(1,pv_ratio)") {
  LiteDivertCfg c = solar_cfg();   // pv_ratio 1.1 -> min(1,1.1)=1.0, so never rounds up
  LiteDivertState st{0};
  LiteDivertResult r = lite_divert_eval(c, st, 2280, 0, 240.0, 0, false, false, 100000); // 9.5 A
  CHECK(r.charge_rate_a == 9);     // 9.5-9=0.5 not > 1.0
  c.pv_ratio = 0.0;                // min(1,0)=0 -> any fraction rounds up
  st.smoothed_available = 0;
  r = lite_divert_eval(c, st, 2280, 0, 240.0, 0, false, false, 100000);
  CHECK(r.charge_rate_a == 10);
}
TEST_CASE("GRID export minus EVSE draw, with reserve from pv_ratio>1") {
  LiteDivertCfg c { LiteDivertType::Grid, 1.1, 20, 600, 6 };
  LiteDivertState st{0};
  // grid_ie = -2400 W exporting / 240 = -10 A; minus evse 0 -> Igrid -10; reserve =
  // 1000*(0.1)/240 = 0.4167 A; available = 10 - 0.4167 = 9.583
  LiteDivertResult r = lite_divert_eval(c, st, 0, -2400, 240.0, 0, false, false, 100000);
  CHECK(r.available == doctest::Approx(9.5833).epsilon(0.001));
  // importing (positive grid_ie) -> no excess -> available 0 -> Hold (not active)
  st.smoothed_available = 0;
  r = lite_divert_eval(c, st, 0, 500, 240.0, 0, false, false, 100000);
  CHECK(r.available == doctest::Approx(0.0));
  CHECK(r.action == LiteDivertAction::Hold);
}
```

- [ ] **Step 3: Run, verify fail** — `pio test -e native -f test_lite_divert` → FAIL.

- [ ] **Step 4: Implement** — `src/lite/lite_divert.cpp`:

```cpp
#include "lite_divert.h"
#include "lite_input_filter.h"
#include <math.h>

LiteDivertResult lite_divert_eval(const LiteDivertCfg &cfg, LiteDivertState &st,
    int solar_w, int grid_ie_w, double voltage, int evse_present_a,
    bool currently_active, bool min_charge_elapsed, uint32_t delta_s)
{
  LiteDivertResult r{};
  double available = 0.0;

  if (cfg.type == LiteDivertType::Grid) {
    double Igrid = (double)grid_ie_w / voltage - (double)evse_present_a; // grid_ie<0 = export
    if (Igrid < 0) {
      double reserve = (1000.0 * ((cfg.pv_ratio > 1.0) ? (cfg.pv_ratio - 1.0) : 0.0)) / voltage;
      available = (-Igrid - reserve);
    }
  } else { // Solar
    available = (double)solar_w / voltage;
  }
  if (available < 0) available = 0.0;

  uint32_t tau = (available > st.smoothed_available) ? cfg.attack_s : cfg.decay_s;
  st.smoothed_available = lite_input_filter(available, st.smoothed_available, tau, delta_s);

  double pvr = (cfg.pv_ratio < 1.0) ? cfg.pv_ratio : 1.0;   // min(1.0, pv_ratio)
  int rate = (int)floor(available);
  if ((available - rate) > pvr) rate += 1;
  double trigger = (double)cfg.min_current_a * pvr;

  r.available = available;
  r.smoothed = st.smoothed_available;
  r.charge_rate_a = rate;

  if (st.smoothed_available >= trigger + LITE_DIVERT_HYSTERESIS) {
    r.action = LiteDivertAction::Charge;
  } else if (st.smoothed_available <= trigger) {
    r.action = (currently_active && min_charge_elapsed) ? LiteDivertAction::Stop
                                                        : LiteDivertAction::Hold;
  } else {
    r.action = LiteDivertAction::Hold;   // inside the hysteresis band
  }
  return r;
}
```

- [ ] **Step 5: Native filter** — append ` +<lite/lite_divert.cpp>` to `[env:native]` `build_src_filter`.

- [ ] **Step 6: Run, verify pass** — `pio test -e native -f test_lite_divert` → PASS.

- [ ] **Step 7: Commit**

```bash
git add src/lite/lite_divert.h src/lite/lite_divert.cpp test/test_lite_divert/ platformio.ini
git commit -m "feat(lite): pure solar-divert decision, parity with upstream divert (native-tested)"
```

---

### Task 5: Divert config persistence + `/config` keys

**Files:** Modify `src/lite/lite_config_store.{h,cpp}`, `src/lite/web_server_lite.cpp`.

- [ ] **Step 1: Config struct** — in `lite_config_store.h`, after `LiteEvseConfig`, add (key names mirror upstream):

```cpp
// Solar-divert config. Persisted as a single FlashDB blob ("divert").
struct LiteDivertConfig {
  bool     enabled;        // divert_enabled (de)
  int      type;           // divert_type (dm): 0=SOLAR, 1=GRID
  double   pv_ratio;       // divert_PV_ratio (dpr)
  uint32_t attack_s;       // divert_attack_smoothing_time (das)
  uint32_t decay_s;        // divert_decay_smoothing_time (dds)
  uint32_t min_charge_s;   // divert_min_charge_time (dt)
};
bool lite_config_load_divert(LiteDivertConfig &out);  // false if key absent (caller uses defaults)
bool lite_config_save_divert(const LiteDivertConfig &in);
```

- [ ] **Step 2: Blob load/save** — in `lite_config_store.cpp`, mirror `lite_config_load_schedule`/`save_schedule` exactly, with key `"divert"`:

```cpp
bool lite_config_load_divert(LiteDivertConfig &out)
{
  if (!s_ready) return false;
  struct fdb_blob blob;
  fdb_kv_get_blob(&s_kvdb, "divert", fdb_blob_make(&blob, &out, sizeof(out)));
  return blob.saved.len == sizeof(out);
}
bool lite_config_save_divert(const LiteDivertConfig &in)
{
  if (!s_ready) return false;
  struct fdb_blob blob;
  return fdb_kv_set_blob(&s_kvdb, "divert",
                         fdb_blob_make(&blob, &in, sizeof(in))) == FDB_NO_ERR;
}
```

- [ ] **Step 3: Cache + defaults in web_server_lite** — near `static LiteEvseConfig s_cfg`, add:

```cpp
static LiteDivertConfig s_divertCfg = { false, 0, 1.1, 20, 600, 600 }; // upstream defaults
```

In `web_server_lite_begin` (where `s_cfg` is seeded from the store), add:

```cpp
  lite_config_load_divert(s_divertCfg);   // keeps defaults if the key is absent
```

- [ ] **Step 4: `/config` read/write** — in `handle_config`, add query-var handling mirroring the `max_current_*` pattern (set on either method, clamp, persist, cache). Add to `config_json` the divert keys so GET echoes them:

```cpp
  // in handle_config, before the `int status = 200;` line:
  LiteDivertConfig dcfg = s_divertCfg; bool dany = false;
  if (mg_get_http_var(&hm->query_string, "divert_enabled", val, sizeof(val)) > 0) { dcfg.enabled = atoi(val) != 0; dany = true; }
  if (mg_get_http_var(&hm->query_string, "divert_type", val, sizeof(val)) > 0)    { dcfg.type = atoi(val) ? 1 : 0; dany = true; }
  { char fv[16];
    if (mg_get_http_var(&hm->query_string, "divert_PV_ratio", fv, sizeof(fv)) > 0)              { dcfg.pv_ratio = atof(fv); dany = true; }
    if (mg_get_http_var(&hm->query_string, "divert_attack_smoothing_time", fv, sizeof(fv)) > 0) { dcfg.attack_s = (uint32_t)atol(fv); dany = true; }
    if (mg_get_http_var(&hm->query_string, "divert_decay_smoothing_time", fv, sizeof(fv)) > 0)  { dcfg.decay_s = (uint32_t)atol(fv); dany = true; }
    if (mg_get_http_var(&hm->query_string, "divert_min_charge_time", fv, sizeof(fv)) > 0)        { dcfg.min_charge_s = (uint32_t)atol(fv); dany = true; }
  }
  if (dany) { lite_config_save_divert(dcfg); s_divertCfg = dcfg; }
```

In `config_json`, add: `doc["divert_enabled"]=s_divertCfg.enabled; doc["divert_type"]=s_divertCfg.type; doc["divert_PV_ratio"]=s_divertCfg.pv_ratio; doc["divert_attack_smoothing_time"]=s_divertCfg.attack_s; doc["divert_decay_smoothing_time"]=s_divertCfg.decay_s; doc["divert_min_charge_time"]=s_divertCfg.min_charge_s;` (bump the `StaticJsonDocument` capacity to fit).

- [ ] **Step 5: Build device env** — `pio run -e openevse_lite` → SUCCESS.

- [ ] **Step 6: Commit**

```bash
git add src/lite/lite_config_store.h src/lite/lite_config_store.cpp src/lite/web_server_lite.cpp
git commit -m "feat(lite): divert config keys (parity names) + FlashDB persistence + /config"
```

---

### Task 6: Divert loop glue — claim via the seam

**Files:** Modify `src/lite/web_server_lite.cpp`.

- [ ] **Step 1: Includes + glue state** — add `#include "lite_divert.h"`. Near the other loop statics, add:

```cpp
static LiteDivertState s_divertState = { 0.0 };
static uint32_t        s_divertLastMs = 0;        // for smoothing delta_s
static uint32_t        s_divertMinChargeEndMs = 0; // millis when min-charge window ends
static bool            s_divertWasCharging = false;
static const uint32_t  FEED_STALE_MS = 120000;    // shared feed staleness (matches sdm default)
```

- [ ] **Step 2: Per-loop divert** — in `web_server_lite_loop`, AFTER the Slice-4 schedule block (ends `s_lastSchedState = st;`) and BEFORE the Slice-3d WS push block, add:

```cpp
  // Solar-divert (autonomous, OpenEVSE-priority). Claims Active@Divert(50) when excess
  // solar is sufficient; Disabled@Default(10) when not (so a schedule/manual still wins);
  // released entirely when divert is disabled. Pure decision in lite_divert_eval.
  if (s_mgr_ctrl && s_divertCfg.enabled) {
    uint32_t now = millis();
    uint32_t delta_s = s_divertLastMs ? (now - s_divertLastMs) / 1000u : 0u;
    s_divertLastMs = now;

    // Feed: pause (fail-safe) when stale -> feed 0 W so divert winds down.
    bool fresh = (s_divertCfg.type == 0)
        ? lite_feed_fresh(s_feed.solar_valid, s_feed.solar_ms, now, FEED_STALE_MS)
        : lite_feed_fresh(s_feed.grid_valid,  s_feed.grid_ms,  now, FEED_STALE_MS);
    int solar_w  = fresh ? s_feed.solar_w  : 0;
    int grid_w   = fresh ? s_feed.grid_ie_w : 0;
    double volts = (s_feed.voltage_valid && s_feed.voltage > 1.0) ? s_feed.voltage : 240.0;

    bool active = (s_mgr_ctrl->getState(EvseClient_OpenEVSE_Divert) == EvseState::Active);
    int present_a = (int)s_mgr_ctrl->getChargeCurrent(); // last resolved/commanded current (getAmps proxy)
    bool min_elapsed = (s_divertMinChargeEndMs == 0) || ((int32_t)(now - s_divertMinChargeEndMs) >= 0);

    LiteDivertCfg dc { (LiteDivertType)s_divertCfg.type, s_divertCfg.pv_ratio,
                       s_divertCfg.attack_s, s_divertCfg.decay_s, 6 };
    LiteDivertResult r = lite_divert_eval(dc, s_divertState, solar_w, grid_w, volts,
                                          present_a, active, min_elapsed, delta_s);

    if (r.action == LiteDivertAction::Charge) {
      EvseProperties p(EvseState::Active);
      p.setChargeCurrent((uint32_t)r.charge_rate_a);
      s_mgr_ctrl->claim(EvseClient_OpenEVSE_Divert, EvseManager_Priority_Divert, p);
    } else if (r.action == LiteDivertAction::Stop) {
      EvseProperties p(EvseState::Disabled);
      s_mgr_ctrl->claim(EvseClient_OpenEVSE_Divert, EvseManager_Priority_Default, p);
    } // Hold: leave the existing claim untouched

    // min-charge timer: arm when charging begins (rising edge of actual charging).
    bool charging = s_mgr_ctrl->isCharging();
    if (charging && !s_divertWasCharging) {
      s_divertMinChargeEndMs = now + s_divertCfg.min_charge_s * 1000u;
      if (s_divertMinChargeEndMs == 0) s_divertMinChargeEndMs = 1; // avoid the "0=disarmed" sentinel
    }
    s_divertWasCharging = charging;
  } else if (s_mgr_ctrl && s_mgr_ctrl->clientHasClaim(EvseClient_OpenEVSE_Divert)) {
    s_mgr_ctrl->release(EvseClient_OpenEVSE_Divert);   // divert turned off -> drop the claim
    s_divertState.smoothed_available = 0;
  }
```

- [ ] **Step 3: Build device env** — `pio run -e openevse_lite` → SUCCESS. Record flash %.

- [ ] **Step 4: Commit**

```bash
git add src/lite/web_server_lite.cpp
git commit -m "feat(lite): wire solar-divert into the loop (claim Active@Divert / Disabled@Default; min-charge + stale-feed)"
```

---

# Sub-slice 3 — Load shaping

### Task 7: `lite_shaper` pure cap (TDD, native)

**Files:** Create `src/lite/lite_shaper.{h,cpp}`, `test/test_lite_shaper/test_lite_shaper.cpp`; Modify `platformio.ini`.

- [ ] **Step 1: Header** — `src/lite/lite_shaper.h`:

```cpp
#pragma once
#include <stdint.h>

struct LiteShaperCfg { uint32_t max_pwr_w; uint32_t smoothing_s; }; // smp, sst
struct LiteShaperState { double smoothed_live_pwr; bool paused; };

// Max-current cap (A) the shaper allows (transcribes CurrentShaperTask::shapeCurrent).
// When not paused, live power is used raw; while paused, rising power is taken immediately,
// falling power is smoothed. When divert is enabled in SOLAR mode, self-production adds to
// the budget (max_pwr += solar). Single-phase. evse_present_a = EVSE's own current.
// Caller treats a cap below the min charge current as "pause".
double lite_shaper_cap(const LiteShaperCfg &cfg, LiteShaperState &st,
    int live_pwr_w, double voltage, int evse_present_a,
    int solar_w, bool divert_solar_enabled, uint32_t delta_s);
```

- [ ] **Step 2: Failing test** — `test/test_lite_shaper/test_lite_shaper.cpp`:

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../src/lite/lite_shaper.h"

TEST_CASE("cap = (max_pwr - livepwr)/V + evse_present, not paused") {
  LiteShaperCfg c { 7200, 60 };           // 7200 W budget
  LiteShaperState st { 0, false };
  // live 4800 W, 240 V, evse drawing 10 A -> (7200-4800)/240 + 10 = 10 + 10 = 20 A
  CHECK(lite_shaper_cap(c, st, 4800, 240.0, 10, 0, false, 5) == doctest::Approx(20.0));
}
TEST_CASE("SOLAR divert adds self-production to the budget") {
  LiteShaperCfg c { 7200, 60 };
  LiteShaperState st { 0, false };
  // +2400 W solar -> (7200+2400-4800)/240 + 0 = 4800/240 = 20 A
  CHECK(lite_shaper_cap(c, st, 4800, 240.0, 0, 2400, true, 5) == doctest::Approx(20.0));
}
TEST_CASE("while paused, rising power is taken immediately") {
  LiteShaperCfg c { 7200, 60 };
  LiteShaperState st { 1000.0, true };     // smoothed starts at 1000
  // live 5000 > smoothed 1000 -> smoothed jumps to 5000 immediately
  lite_shaper_cap(c, st, 5000, 240.0, 0, 0, false, 5);
  CHECK(st.smoothed_live_pwr == doctest::Approx(5000.0));
}
TEST_CASE("while paused, falling power is smoothed") {
  LiteShaperCfg c { 7200, 60 };
  LiteShaperState st { 5000.0, true };
  // live 1000 < smoothed 5000 -> EMA toward 1000 (does not jump)
  lite_shaper_cap(c, st, 1000, 240.0, 0, 0, false, 5);
  CHECK(st.smoothed_live_pwr > 1000.0);
  CHECK(st.smoothed_live_pwr < 5000.0);
}
```

- [ ] **Step 3: Run, verify fail** — `pio test -e native -f test_lite_shaper` → FAIL.

- [ ] **Step 4: Implement** — `src/lite/lite_shaper.cpp`:

```cpp
#include "lite_shaper.h"
#include "lite_input_filter.h"

double lite_shaper_cap(const LiteShaperCfg &cfg, LiteShaperState &st,
    int live_pwr_w, double voltage, int evse_present_a,
    int solar_w, bool divert_solar_enabled, uint32_t delta_s)
{
  int max_pwr = (int)cfg.max_pwr_w;
  int livepwr;
  if (!st.paused) {
    st.smoothed_live_pwr = live_pwr_w;
    livepwr = live_pwr_w;
  } else {
    if (live_pwr_w > st.smoothed_live_pwr) {
      st.smoothed_live_pwr = live_pwr_w;
    } else {
      st.smoothed_live_pwr = lite_input_filter(live_pwr_w, st.smoothed_live_pwr,
                                               cfg.smoothing_s, delta_s);
    }
    livepwr = (int)st.smoothed_live_pwr;
  }
  if (divert_solar_enabled) max_pwr += solar_w;     // upstream adds self-production in SOLAR mode
  return ((double)(max_pwr - livepwr) / voltage) + (double)evse_present_a;
}
```

- [ ] **Step 5: Native filter** — append ` +<lite/lite_shaper.cpp>` to `[env:native]` `build_src_filter`.

- [ ] **Step 6: Run, verify pass** — `pio test -e native -f test_lite_shaper` → PASS.

- [ ] **Step 7: Commit**

```bash
git add src/lite/lite_shaper.h src/lite/lite_shaper.cpp test/test_lite_shaper/ platformio.ini
git commit -m "feat(lite): pure load-shaper cap, parity with upstream current_shaper (native-tested)"
```

---

### Task 8: Shaper config persistence + `/config` keys

**Files:** Modify `src/lite/lite_config_store.{h,cpp}`, `src/lite/web_server_lite.cpp`.

- [ ] **Step 1: Config struct** — in `lite_config_store.h`, add:

```cpp
// Load-shaper config. Persisted as a single FlashDB blob ("shaper").
struct LiteShaperConfig {
  bool     enabled;            // current_shaper_enabled (se)
  uint32_t max_pwr_w;          // current_shaper_max_pwr (smp)
  uint32_t smoothing_s;        // current_shaper_smoothing_time (sst)
  uint32_t data_maxinterval_s; // current_shaper_data_maxinterval (sdm)
  uint32_t min_pause_s;        // current_shaper_min_pause_time (spt)
};
bool lite_config_load_shaper(LiteShaperConfig &out);
bool lite_config_save_shaper(const LiteShaperConfig &in);
```

- [ ] **Step 2: Blob load/save** — in `lite_config_store.cpp`, mirror the schedule blob functions with key `"shaper"`:

```cpp
bool lite_config_load_shaper(LiteShaperConfig &out)
{
  if (!s_ready) return false;
  struct fdb_blob blob;
  fdb_kv_get_blob(&s_kvdb, "shaper", fdb_blob_make(&blob, &out, sizeof(out)));
  return blob.saved.len == sizeof(out);
}
bool lite_config_save_shaper(const LiteShaperConfig &in)
{
  if (!s_ready) return false;
  struct fdb_blob blob;
  return fdb_kv_set_blob(&s_kvdb, "shaper",
                         fdb_blob_make(&blob, &in, sizeof(in))) == FDB_NO_ERR;
}
```

- [ ] **Step 3: Cache + defaults** — near `s_divertCfg`, add:

```cpp
static LiteShaperConfig s_shaperCfg = { false, 0, 60, 120, 300 }; // upstream defaults
```

In `web_server_lite_begin`, add `lite_config_load_shaper(s_shaperCfg);`.

- [ ] **Step 4: `/config` read/write** — in `handle_config`, add (mirroring Task 5 Step 4):

```cpp
  LiteShaperConfig scfg = s_shaperCfg; bool sany = false;
  if (mg_get_http_var(&hm->query_string, "current_shaper_enabled", val, sizeof(val)) > 0) { scfg.enabled = atoi(val) != 0; sany = true; }
  { char fv[16];
    if (mg_get_http_var(&hm->query_string, "current_shaper_max_pwr", fv, sizeof(fv)) > 0)          { scfg.max_pwr_w = (uint32_t)atol(fv); sany = true; }
    if (mg_get_http_var(&hm->query_string, "current_shaper_smoothing_time", fv, sizeof(fv)) > 0)   { scfg.smoothing_s = (uint32_t)atol(fv); sany = true; }
    if (mg_get_http_var(&hm->query_string, "current_shaper_data_maxinterval", fv, sizeof(fv)) > 0) { scfg.data_maxinterval_s = (uint32_t)atol(fv); sany = true; }
    if (mg_get_http_var(&hm->query_string, "current_shaper_min_pause_time", fv, sizeof(fv)) > 0)   { scfg.min_pause_s = (uint32_t)atol(fv); sany = true; }
  }
  if (sany) { lite_config_save_shaper(scfg); s_shaperCfg = scfg; }
```

Add the five keys to `config_json` (bump `StaticJsonDocument` capacity).

- [ ] **Step 5: Build device env** — `pio run -e openevse_lite` → SUCCESS.

- [ ] **Step 6: Commit**

```bash
git add src/lite/lite_config_store.h src/lite/lite_config_store.cpp src/lite/web_server_lite.cpp
git commit -m "feat(lite): shaper config keys (parity names) + FlashDB persistence + /config"
```

---

### Task 9: Shaper loop glue — MaxCurrent cap @ Safety + pause + stale

**Files:** Modify `src/lite/web_server_lite.cpp`.

- [ ] **Step 1: Includes + glue state** — add `#include "lite_shaper.h"`. Near the other loop statics add:

```cpp
static LiteShaperState s_shaperState = { 0.0, false };
static uint32_t        s_shaperLastMs   = 0;  // smoothing delta_s
static uint32_t        s_shaperPauseMs  = 0;  // millis when current pause began (0 = not paused)
static const double    LITE_SHAPER_HYSTERESIS = 0.5; // A
static const int       LITE_MIN_CURRENT = 6;         // J1772 floor
```

- [ ] **Step 2: Per-loop shaper** — in `web_server_lite_loop`, immediately BEFORE the divert block from Task 6 (shaper must apply on top of divert's claim via its higher Safety priority; order of claim calls doesn't matter since arbitrate is priority-based, but compute the cap each tick), add:

```cpp
  // Load-shaper: cap total current to the site-power budget via a MaxCurrent claim at
  // Safety (caps every client incl. manual). Pauses (Disabled@Limit) when the cap drops
  // below the min current or the shaper feed goes stale. Pure cap in lite_shaper_cap.
  if (s_mgr_ctrl && s_shaperCfg.enabled) {
    uint32_t now = millis();
    uint32_t delta_s = s_shaperLastMs ? (now - s_shaperLastMs) / 1000u : 0u;
    s_shaperLastMs = now;

    bool fresh = lite_feed_fresh(s_feed.shaper_valid, s_feed.shaper_ms, now,
                                 s_shaperCfg.data_maxinterval_s * 1000u);
    double volts = (s_feed.voltage_valid && s_feed.voltage > 1.0) ? s_feed.voltage : 240.0;

    if (!fresh) {
      // Stale feed -> pause charge (Disabled@Limit), arm pause timer, freeze smoothing.
      if (!s_shaperPauseMs) s_shaperPauseMs = now;
      s_shaperState.paused = true;
      if (s_mgr_ctrl->getState(EvseClient_OpenEVSE_Shaper) != EvseState::Disabled) {
        EvseProperties p(EvseState::Disabled);
        s_mgr_ctrl->claim(EvseClient_OpenEVSE_Shaper, EvseManager_Priority_Limit, p);
      }
    } else {
      int present_a = (int)s_mgr_ctrl->getChargeCurrent();
      double cap = lite_shaper_cap({ s_shaperCfg.max_pwr_w, s_shaperCfg.smoothing_s },
                                   s_shaperState, s_feed.shaper_w, volts, present_a,
                                   s_feed.solar_w, s_divertCfg.enabled && s_divertCfg.type == 0,
                                   delta_s);
      EvseProperties p;
      p.setMaxCurrent((uint32_t)(cap < 0 ? 0 : (uint32_t)cap)); // floor()/clamp; <6 handled below
      if (cap < LITE_MIN_CURRENT) {
        p.setState(EvseState::Disabled);            // not enough headroom -> pause
        if (!s_shaperPauseMs) s_shaperPauseMs = now;
        s_shaperState.paused = true;
      } else if (s_shaperPauseMs &&
                 (now - s_shaperPauseMs) >= s_shaperCfg.min_pause_s * 1000u &&
                 (cap - LITE_MIN_CURRENT) >= LITE_SHAPER_HYSTERESIS) {
        s_shaperPauseMs = 0;                        // resume after min-pause + hysteresis
        s_shaperState.paused = false;
      }
      s_mgr_ctrl->claim(EvseClient_OpenEVSE_Shaper, EvseManager_Priority_Safety, p);
    }
  } else if (s_mgr_ctrl && s_mgr_ctrl->clientHasClaim(EvseClient_OpenEVSE_Shaper)) {
    s_mgr_ctrl->release(EvseClient_OpenEVSE_Shaper);
    s_shaperState.smoothed_live_pwr = 0; s_shaperPauseMs = 0; s_shaperState.paused = false;
  }
```

- [ ] **Step 3: Build device env** — `pio run -e openevse_lite` → SUCCESS. Record flash %.

- [ ] **Step 4: Commit**

```bash
git add src/lite/web_server_lite.cpp
git commit -m "feat(lite): wire load-shaper into the loop (MaxCurrent@Safety + pause/resume + stale-feed)"
```

---

### Task 10: Full verification

**Files:** none (verification only).

- [ ] **Step 1: Full native suite** — `pio test -e native` → ALL PASS (incl. the four new suites).

- [ ] **Step 2: Production build** — `pio run -e openevse_lite` → SUCCESS. Record final flash bytes + % of the 960 KB OTA slot (983040 B); confirm well under (baseline was 505024 B / 51.4%; expected ~+15–30 KB).

- [ ] **Step 3: Route/glue sanity** — `grep -nE 'status_post_apply|EvseClient_OpenEVSE_Divert|EvseClient_OpenEVSE_Shaper|EvseManager_Priority_Safety|lite_divert_eval|lite_shaper_cap' src/lite/web_server_lite.cpp` — confirm: POST receiver present, both claims present, shaper at Safety, both pure evals called from the loop.

- [ ] **Step 4: Acceptance check (WiFi, no charge control needed)** — flash a unit; `POST /status` with `{"solar":3000,"voltage":240}` then `GET /status` → confirm `solar`/`voltage` echo back; set `divert_enabled=1` via `/config` and confirm it persists across a reboot (`GET /config`). NOTE: charge-current actuation (divert claiming, shaper capping the `$AL` setpoint) **cannot be validated on the GFI-faulted bench** — defer to a complete unit with a real load + live solar/grid feed (same ceiling as override/schedule charge control). Echo + config persistence + native parity tests are the acceptance bar for this branch.

---

## Notes for the implementer

- **Parity is the spec.** The pure-unit code above is transcribed from `divert.cpp` /
  `current_shaper.cpp` / `input_filter.cpp` in the standard firmware (`/home/rar/oevse/openevse_esp32_firmware/src/`). If a test disagrees with upstream behavior, re-read the upstream source — it's ground truth.
- **`EvseProperties` defaults** leave charge/max current as `UINT32_MAX` (= "unset"); only set the field a claim means to assert (divert sets `ChargeCurrent`; shaper sets `MaxCurrent`). `lite_evse_arbitrate` resolves each field by highest priority; `LiteEvseManager::apply()` clamps `charge_current` to `max_current` then to hardware via `lite_clamp_charge_current` (6 A floor).
- **No new web UI** — config is set via `/config` query params (Slice 5 deferred).
- **Commits:** author `Andrew Rankin <andrewrankin@gmail.com>`; never add a Co-Authored-By trailer.
