# Lite Wall-Clock + Lifetime Energy Totals Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the JuiceBox-lite firmware a network-synced wall clock and persistent lifetime energy totals (day/week/month/year), exposed in `GET /status` in the OpenEVSE shape the firstof9/openevse HA integration consumes.

**Architecture:** Two pure, native-testable cores — `LiteClock` (software wall-clock seeded by SNTP) and `lite_energy_totals` (calendar-bucketed Wh accumulator) — plus thin glue: Mongoose's built-in SNTP client (enabled via build flag) syncs the clock on the existing `mg_mgr_poll` loop; `main_lite` accrues a finished session's Wh into the totals on the charge→idle edge and persists them to FlashDB; `web_server_lite` emits `time` + `total_*`.

**Tech Stack:** C++ (gnu++17 native / gnu++14 device), doctest (native tests), MongooseLite SNTP (`mg_sntp_get_time`), FlashDB KVDB (persistence), LibreTiny/WGM160P (device).

---

## File Structure

- **`src/lite/lite_clock.{h,cpp}`** (new, pure) — `LiteClock` class (epoch+millis wall-clock) plus free helpers `lite_civil_from_secs()` (epoch→Y/M/D) and `lite_clock_iso8601()` (epoch→`YYYY-MM-DDTHH:MM:SSZ`). No Arduino/network/flash. Civil math lives here because it is the time domain; `lite_energy_totals` reuses it.
- **`src/lite/lite_energy_totals.{h,cpp}`** (new, pure) — `LiteEnergyTotals` POD struct + `energy_totals_init/add()` + period-id helpers. Includes `lite_clock.h` for civil math. No Arduino/network/flash.
- **`test/test_lite_clock/test_lite_clock.cpp`** (new) — doctest for the clock + civil + ISO helpers.
- **`test/test_lite_energy_totals/test_lite_energy_totals.cpp`** (new) — doctest for accrual + rollover + period ids.
- **`src/lite/lite_config_store.{h,cpp}`** (modify) — persist `LiteEnergyTotals` (one blob) + clock config (`sntp_hostname` string, `tz_offset_min` int).
- **`src/lite/web_server_lite.cpp`** (modify) — enable + drive SNTP sync on the poll loop; emit `time`/`total_*` in `/status`; accept `tz_offset_min`/`sntp_hostname` in `/config`; `web_server_lite_begin` takes clock + totals refs.
- **`src/lite/web_server_lite.h`** (modify) — updated `web_server_lite_begin` signature.
- **`src/lite/main_lite.cpp`** (modify) — own `LiteClock`/`LiteEnergyTotals`; load config + totals; poll the charge→idle edge to accrue + persist.
- **`platformio.ini`** (modify) — `-D MG_ENABLE_SNTP=1` on `[env:openevse_lite]`; add the two new `.cpp` to `[env:native]` `build_src_filter`.

---

### Task 1: `LiteClock` pure unit (wall-clock + civil + ISO helpers)

**Files:**
- Create: `src/lite/lite_clock.h`, `src/lite/lite_clock.cpp`
- Test: `test/test_lite_clock/test_lite_clock.cpp`
- Modify: `platformio.ini` (`[env:native]` `build_src_filter`)

- [ ] **Step 1: Write the failing test**

Create `test/test_lite_clock/test_lite_clock.cpp`:

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../src/lite/lite_clock.h"
#include <cstring>

TEST_CASE("nowUtc advances with millis from the synced epoch") {
  LiteClock c;
  CHECK_FALSE(c.valid());
  CHECK(c.nowUtc(123456) == 0u);          // invalid before first sync
  c.setEpoch(1735689600u, 1000u);         // 2025-01-01 00:00:00 UTC at ms=1000
  CHECK(c.valid());
  CHECK(c.nowUtc(1000u)    == 1735689600u);
  CHECK(c.nowUtc(31000u)   == 1735689630u); // +30 s
  CHECK(c.nowUtc(1000u + 3600000u) == 1735689600u + 3600u); // +1 h
}

TEST_CASE("nowUtc is correct across a 32-bit millis wrap") {
  LiteClock c;
  c.setEpoch(1735689600u, 0xFFFFF000u);   // synced just before wrap
  uint32_t afterWrap = 0xFFFFF000u + 5000u; // wraps past 2^32
  CHECK(c.nowUtc(afterWrap) == 1735689600u + 5u);
}

TEST_CASE("tz offset shifts local time only") {
  LiteClock c;
  c.setEpoch(1735689600u, 0u);
  c.setTzOffsetMinutes(-300);             // UTC-5
  CHECK(c.nowUtc(0u)   == 1735689600u);
  CHECK(c.nowLocal(0u) == 1735689600u - 300u * 60u);
  CHECK(c.tzOffsetMinutes() == -300);
}

TEST_CASE("resyncDue: always due before first sync, then after the interval") {
  LiteClock c;
  CHECK(c.resyncDue(0u));                  // never synced
  c.setEpoch(1735689600u, 1000u);
  CHECK_FALSE(c.resyncDue(1000u));
  CHECK_FALSE(c.resyncDue(1000u + LiteClock::RESYNC_INTERVAL_MS - 1u));
  CHECK(c.resyncDue(1000u + LiteClock::RESYNC_INTERVAL_MS));
}

TEST_CASE("civil_from_secs decodes known dates") {
  int y; unsigned m, d;
  lite_civil_from_secs(1735689600u, y, m, d);  // 2025-01-01
  CHECK(y == 2025); CHECK(m == 1u); CHECK(d == 1u);
  lite_civil_from_secs(1738368000u, y, m, d);  // 2025-02-01
  CHECK(y == 2025); CHECK(m == 2u); CHECK(d == 1u);
  lite_civil_from_secs(1767225600u, y, m, d);  // 2026-01-01
  CHECK(y == 2026); CHECK(m == 1u); CHECK(d == 1u);
}

TEST_CASE("iso8601 formats UTC epoch") {
  char buf[24];
  lite_clock_iso8601(1735689600u, buf, sizeof(buf));   // 2025-01-01 00:00:00
  CHECK(strcmp(buf, "2025-01-01T00:00:00Z") == 0);
  lite_clock_iso8601(1735689600u + 3661u, buf, sizeof(buf)); // +01:01:01
  CHECK(strcmp(buf, "2025-01-01T01:01:01Z") == 0);
}
```

- [ ] **Step 2: Add the new source to the native build, run, verify it FAILS to compile**

Edit `platformio.ini` `[env:native]` `build_src_filter` — append `+<lite/lite_clock.cpp>` to the existing line so it reads (keep all existing entries, add the new one at the end):

```
build_src_filter = -<*> +<tsdb_sample.cpp> +<home_battery.cpp> +<lite/lite_random.cpp> +<lite/juicebox_proto.cpp> +<lite/lite_charge_policy.cpp> +<lite/lite_evse_properties.cpp> +<lite/lite_evse_arbitrate.cpp> +<lite/lite_session_energy.cpp> +<lite/lite_openevse_compat.cpp> +<lite/lite_clock.cpp>
```

Run: `pio test -e native -f test_lite_clock`
Expected: FAIL — `lite_clock.h: No such file or directory`.

- [ ] **Step 3: Write `src/lite/lite_clock.h`**

```cpp
#pragma once
#include <stdint.h>
#include <stddef.h>

// Pure software wall-clock. Seeded from an SNTP UTC epoch; free-runs on a millis()-style
// monotonic clock between syncs. No Arduino / network / flash deps -> native-testable.
class LiteClock {
public:
  static const uint32_t RESYNC_INTERVAL_MS = 3600000UL; // re-sync hourly

  void setEpoch(uint32_t utcSeconds, uint32_t nowMs) {
    _epochAtSync = utcSeconds; _msAtSync = nowMs; _valid = true;
  }
  bool valid() const { return _valid; }

  // UTC seconds = synced epoch + whole seconds elapsed since the sync. Unsigned
  // subtraction makes the millis() delta correct across a 32-bit (~49.7 d) wrap.
  uint32_t nowUtc(uint32_t nowMs) const {
    if (!_valid) return 0;
    return _epochAtSync + (uint32_t)(nowMs - _msAtSync) / 1000u;
  }
  uint32_t nowLocal(uint32_t nowMs) const {
    if (!_valid) return 0;
    return (uint32_t)((int32_t)nowUtc(nowMs) + _tzOffsetMin * 60);
  }
  void setTzOffsetMinutes(int minutes) { _tzOffsetMin = minutes; }
  int  tzOffsetMinutes() const { return _tzOffsetMin; }

  bool resyncDue(uint32_t nowMs) const {
    if (!_valid) return true;
    return (uint32_t)(nowMs - _msAtSync) >= RESYNC_INTERVAL_MS;
  }

private:
  uint32_t _epochAtSync = 0;
  uint32_t _msAtSync    = 0;
  int      _tzOffsetMin = 0;
  bool     _valid       = false;
};

// Epoch seconds -> civil date (proleptic Gregorian). Howard Hinnant's algorithm.
void lite_civil_from_secs(uint32_t epoch, int &year, unsigned &month, unsigned &day);

// Epoch seconds -> "YYYY-MM-DDTHH:MM:SSZ". buf must be >= 21 bytes. Always NUL-terminates.
void lite_clock_iso8601(uint32_t epoch, char *buf, size_t cap);
```

- [ ] **Step 4: Write `src/lite/lite_clock.cpp`**

```cpp
#include "lite_clock.h"
#include <stdio.h>

void lite_civil_from_secs(uint32_t epoch, int &year, unsigned &month, unsigned &day) {
  int32_t  z   = (int32_t)(epoch / 86400u) + 719468;          // shift epoch to 0000-03-01
  int32_t  era = (z >= 0 ? z : z - 146096) / 146097;
  uint32_t doe = (uint32_t)(z - era * 146097);                // [0, 146096]
  uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; // [0, 399]
  int32_t  y   = (int32_t)yoe + era * 400;
  uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);     // [0, 365]
  uint32_t mp  = (5 * doy + 2) / 153;                          // [0, 11]
  day   = doy - (153 * mp + 2) / 5 + 1;                        // [1, 31]
  month = mp < 10 ? mp + 3 : mp - 9;                           // [1, 12]
  year  = y + (month <= 2);
}

void lite_clock_iso8601(uint32_t epoch, char *buf, size_t cap) {
  int y; unsigned mo, d;
  lite_civil_from_secs(epoch, y, mo, d);
  uint32_t sod = epoch % 86400u;                               // seconds of day
  unsigned hh = (unsigned)(sod / 3600u), mm = (unsigned)((sod % 3600u) / 60u), ss = (unsigned)(sod % 60u);
  snprintf(buf, cap, "%04d-%02u-%02uT%02u:%02u:%02uZ", y, mo, d, hh, mm, ss);
}
```

- [ ] **Step 5: Run the test, verify PASS**

Run: `pio test -e native -f test_lite_clock`
Expected: PASS (6 test cases).

- [ ] **Step 6: Commit**

```bash
git add src/lite/lite_clock.h src/lite/lite_clock.cpp test/test_lite_clock/ platformio.ini
git commit -m "feat(lite): LiteClock pure wall-clock + civil/ISO8601 helpers (native-tested)"
```

---

### Task 2: `lite_energy_totals` pure unit (calendar-bucketed accumulator)

**Files:**
- Create: `src/lite/lite_energy_totals.h`, `src/lite/lite_energy_totals.cpp`
- Test: `test/test_lite_energy_totals/test_lite_energy_totals.cpp`
- Modify: `platformio.ini` (`[env:native]` `build_src_filter`)

- [ ] **Step 1: Write the failing test**

Create `test/test_lite_energy_totals/test_lite_energy_totals.cpp`:

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../src/lite/lite_energy_totals.h"

static const uint32_t JAN01 = 1735689600u; // 2025-01-01 00:00:00 UTC
static const uint32_t JAN02 = JAN01 + 86400u;
static const uint32_t FEB01 = 1738368000u; // 2025-02-01
static const uint32_t Y2026 = 1767225600u; // 2026-01-01

TEST_CASE("init zeroes totals and marks period ids unset") {
  LiteEnergyTotals t; energy_totals_init(t);
  CHECK(t.lifetime_wh == 0u); CHECK(t.switches == 0u);
  CHECK(t.day_id == -1); CHECK(t.year_id == -1);
}

TEST_CASE("a session adds to every bucket and counts a switch") {
  LiteEnergyTotals t; energy_totals_init(t);
  energy_totals_add(t, 1500u, JAN01, true);
  CHECK(t.lifetime_wh == 1500u);
  CHECK(t.day_wh == 1500u); CHECK(t.week_wh == 1500u);
  CHECK(t.month_wh == 1500u); CHECK(t.year_wh == 1500u);
  CHECK(t.switches == 1u);
}

TEST_CASE("crossing a day boundary resets the day bucket only") {
  LiteEnergyTotals t; energy_totals_init(t);
  energy_totals_add(t, 1000u, JAN01, true);
  energy_totals_add(t, 400u,  JAN02, true);   // next day, same week/month/year
  CHECK(t.day_wh   == 400u);                  // reset, then +400
  CHECK(t.week_wh  == 1400u);                 // same week -> accumulates
  CHECK(t.month_wh == 1400u);
  CHECK(t.year_wh  == 1400u);
  CHECK(t.lifetime_wh == 1400u);
  CHECK(t.switches == 2u);
}

TEST_CASE("crossing month and year boundaries resets those buckets") {
  LiteEnergyTotals t; energy_totals_init(t);
  energy_totals_add(t, 1000u, JAN01, true);
  energy_totals_add(t, 700u,  FEB01, true);   // new month, same year
  CHECK(t.month_wh == 700u);
  CHECK(t.year_wh  == 1700u);
  energy_totals_add(t, 200u,  Y2026, true);   // new year
  CHECK(t.year_wh  == 200u);
  CHECK(t.lifetime_wh == 1900u);
}

TEST_CASE("pre-sync sessions add to lifetime only (no calendar bucket)") {
  LiteEnergyTotals t; energy_totals_init(t);
  energy_totals_add(t, 800u, 0u, false);      // clock not valid
  CHECK(t.lifetime_wh == 800u);
  CHECK(t.switches == 1u);
  CHECK(t.day_wh == 0u); CHECK(t.day_id == -1);  // untouched
}

TEST_CASE("period ids: day, Monday-week, month, year") {
  CHECK(energy_period_day(JAN01)   == (int32_t)(JAN01 / 86400u));
  CHECK(energy_period_month(JAN01) == 2025 * 12 + 0);
  CHECK(energy_period_month(FEB01) == 2025 * 12 + 1);
  CHECK(energy_period_year(JAN01)  == 2025);
  CHECK(energy_period_year(Y2026)  == 2026);
  // 2025-01-01 is a Wednesday; its Monday-aligned week == that of 2024-12-30..2025-01-05.
  CHECK(energy_period_week(JAN01) == energy_period_week(JAN01 + 2u * 86400u)); // Wed & Fri same week
  CHECK(energy_period_week(JAN01) != energy_period_week(JAN01 + 5u * 86400u)); // next Monday differs
}
```

- [ ] **Step 2: Add to native build, run, verify FAIL**

Edit `platformio.ini` `[env:native]` `build_src_filter` — append `+<lite/lite_energy_totals.cpp>` to the end of the line.

Run: `pio test -e native -f test_lite_energy_totals`
Expected: FAIL — `lite_energy_totals.h: No such file or directory`.

- [ ] **Step 3: Write `src/lite/lite_energy_totals.h`**

```cpp
#pragma once
#include <stdint.h>
#include "lite_clock.h"   // lite_civil_from_secs for month/year bucketing

// Persistent lifetime energy totals, bucketed by local calendar period. POD: persisted
// verbatim as one FlashDB blob. Wh internally (uint64 -> no overflow); kWh emitted at /status.
struct LiteEnergyTotals {
  uint64_t lifetime_wh;
  uint64_t day_wh, week_wh, month_wh, year_wh;
  int32_t  day_id, week_id, month_id, year_id;  // local-calendar period ids; -1 = unset
  uint32_t switches;                            // completed-session count
};

void energy_totals_init(LiteEnergyTotals &t);   // zero + ids = -1

// Add a completed session's Wh at local-time `localEpoch`. Resets any bucket whose period
// id changed since the last add, then adds. If !clockValid, adds to lifetime + switches only
// (no calendar bucketing). Always increments switches.
void energy_totals_add(LiteEnergyTotals &t, uint32_t sessionWh, uint32_t localEpoch, bool clockValid);

// Local-calendar period ids (pure). day/week need no civil math; week is Monday-aligned.
int32_t energy_period_day(uint32_t localEpoch);
int32_t energy_period_week(uint32_t localEpoch);
int32_t energy_period_month(uint32_t localEpoch);
int32_t energy_period_year(uint32_t localEpoch);
```

- [ ] **Step 4: Write `src/lite/lite_energy_totals.cpp`**

```cpp
#include "lite_energy_totals.h"

int32_t energy_period_day(uint32_t localEpoch)  { return (int32_t)(localEpoch / 86400u); }
// 1970-01-01 was a Thursday; +3 days shifts the floor boundary to Monday.
int32_t energy_period_week(uint32_t localEpoch) { return (int32_t)((localEpoch / 86400u + 3u) / 7u); }
int32_t energy_period_month(uint32_t localEpoch) {
  int y; unsigned m, d; lite_civil_from_secs(localEpoch, y, m, d);
  return y * 12 + (int32_t)(m - 1);
}
int32_t energy_period_year(uint32_t localEpoch) {
  int y; unsigned m, d; lite_civil_from_secs(localEpoch, y, m, d);
  return y;
}

void energy_totals_init(LiteEnergyTotals &t) {
  t.lifetime_wh = t.day_wh = t.week_wh = t.month_wh = t.year_wh = 0;
  t.day_id = t.week_id = t.month_id = t.year_id = -1;
  t.switches = 0;
}

void energy_totals_add(LiteEnergyTotals &t, uint32_t sessionWh, uint32_t localEpoch, bool clockValid) {
  t.lifetime_wh += sessionWh;
  t.switches    += 1;
  if (!clockValid) return;   // no wall clock yet -> lifetime only

  int32_t d  = energy_period_day(localEpoch);
  int32_t w  = energy_period_week(localEpoch);
  int32_t mo = energy_period_month(localEpoch);
  int32_t yr = energy_period_year(localEpoch);

  if (t.day_id   != d)  { t.day_wh   = 0; t.day_id   = d;  }
  if (t.week_id  != w)  { t.week_wh  = 0; t.week_id  = w;  }
  if (t.month_id != mo) { t.month_wh = 0; t.month_id = mo; }
  if (t.year_id  != yr) { t.year_wh  = 0; t.year_id  = yr; }

  t.day_wh   += sessionWh;
  t.week_wh  += sessionWh;
  t.month_wh += sessionWh;
  t.year_wh  += sessionWh;
}
```

- [ ] **Step 5: Run the test, verify PASS**

Run: `pio test -e native -f test_lite_energy_totals`
Expected: PASS (6 test cases).

- [ ] **Step 6: Commit**

```bash
git add src/lite/lite_energy_totals.h src/lite/lite_energy_totals.cpp test/test_lite_energy_totals/ platformio.ini
git commit -m "feat(lite): lite_energy_totals pure calendar-bucketed accumulator (native-tested)"
```

---

### Task 3: Persist totals + clock config in the FlashDB config store

**Files:**
- Modify: `src/lite/lite_config_store.h`, `src/lite/lite_config_store.cpp`

This is device-only (FlashDB isn't in the native build); verified by the device build in Task 7. Follow the existing `kv_get_*/kv_set_*` blob pattern.

- [ ] **Step 1: Declare the new load/save API in `src/lite/lite_config_store.h`**

Add `#include "lite_energy_totals.h"` near the top (after the existing includes), and inside the `#ifdef OPENEVSE_LITE` block add:

```cpp
// Clock config (mirrors upstream keys: sntp_hostname/"sh", time_zone offset).
struct LiteClockConfig { String sntp_hostname; int tz_offset_min; };

bool lite_config_load_totals(LiteEnergyTotals &out);   // false if key absent (caller inits)
bool lite_config_save_totals(const LiteEnergyTotals &in);

bool lite_config_load_clock(LiteClockConfig &out);     // fills defaults if keys absent
bool lite_config_save_clock(const LiteClockConfig &in);
```

- [ ] **Step 2: Implement in `src/lite/lite_config_store.cpp`**

Add `#include "lite_energy_totals.h"` near the top. Add these functions (they use the existing `s_ready`, `s_kvdb`, and `kv_get_str/kv_set_str/kv_get_int/kv_set_int` helpers):

```cpp
bool lite_config_load_totals(LiteEnergyTotals &out) {
  if (!s_ready) return false;
  struct fdb_blob blob;
  fdb_kv_get_blob(&s_kvdb, "energy_totals", fdb_blob_make(&blob, &out, sizeof(out)));
  return blob.saved.len == sizeof(out);   // full struct present
}

bool lite_config_save_totals(const LiteEnergyTotals &in) {
  if (!s_ready) return false;
  struct fdb_blob blob;
  return fdb_kv_set_blob(&s_kvdb, "energy_totals",
                         fdb_blob_make(&blob, &in, sizeof(in))) == FDB_NO_ERR;
}

bool lite_config_load_clock(LiteClockConfig &out) {
  out.sntp_hostname = "pool.ntp.org";   // defaults
  out.tz_offset_min = 0;
  if (!s_ready) return false;
  kv_get_str("sntp_hostname", out.sntp_hostname);
  kv_get_int("tz_offset_min", out.tz_offset_min);
  return true;
}

bool lite_config_save_clock(const LiteClockConfig &in) {
  if (!s_ready) return false;
  bool ok = kv_set_str("sntp_hostname", in.sntp_hostname);
  ok = kv_set_int("tz_offset_min", in.tz_offset_min) && ok;
  return ok;
}
```

- [ ] **Step 3: Build the device env to verify it compiles**

Run: `pio run -e openevse_lite 2>&1 | tail -3`
Expected: `[SUCCESS]`.

- [ ] **Step 4: Commit**

```bash
git add src/lite/lite_config_store.h src/lite/lite_config_store.cpp
git commit -m "feat(lite): persist energy totals + clock config (sntp_hostname, tz offset)"
```

---

### Task 4: Enable Mongoose SNTP + drive sync on the poll loop

**Files:**
- Modify: `platformio.ini` (`[env:openevse_lite]` build_flags)
- Modify: `src/lite/web_server_lite.h`, `src/lite/web_server_lite.cpp`

- [ ] **Step 1: Enable SNTP in the build**

Edit `platformio.ini` `[env:openevse_lite]` `build_flags` — add the line `-D MG_ENABLE_SNTP=1` alongside the existing `-D MG_ENABLE_SSL=0` (same block).

- [ ] **Step 2: Update `web_server_lite_begin` signature in `src/lite/web_server_lite.h`**

Change the declaration to take the clock + totals (forward-declare the types):

```cpp
#pragma once
class LiteEvseManager;
class LiteClock;
struct LiteEnergyTotals;
void web_server_lite_begin(LiteEvseManager &mgr, LiteClock &clock, LiteEnergyTotals &totals);
void web_server_lite_loop();
```

- [ ] **Step 3: Wire SNTP into `src/lite/web_server_lite.cpp`**

Near the existing static handles (where `s_mgr_ctrl` is declared, ~line 35), add includes and statics:

```cpp
#include "lite_clock.h"
#include "lite_energy_totals.h"
#include "lite_config_store.h"

static LiteClock        *s_clock  = nullptr;
static LiteEnergyTotals *s_totals = nullptr;
static String            s_sntpHost = "pool.ntp.org";
static unsigned long     s_lastSntpAttemptMs = 0;
static const unsigned long SNTP_RETRY_MS = 30000;  // re-attempt cadence while unsynced/resync-due
```

Add the SNTP reply handler (above `ev_handler`):

```cpp
// Mongoose SNTP callback: a reply carries unix time as a double in mg_sntp_message.time.
static void sntp_ev_handler(struct mg_connection *nc, int ev, void *p, void *user_data) {
  (void)nc; (void)user_data;
  if (ev == MG_SNTP_REPLY && s_clock) {
    struct mg_sntp_message *m = (struct mg_sntp_message *)p;
    s_clock->setEpoch((uint32_t)m->time, millis());
  }
}
```

In `web_server_lite_begin`, accept and stash the refs + seed the SNTP host from config (replace the existing signature + opening lines):

```cpp
void web_server_lite_begin(LiteEvseManager &mgr, LiteClock &clock, LiteEnergyTotals &totals)
{
  s_mgr_ctrl = &mgr;
  s_clock    = &clock;
  s_totals   = &totals;

  LiteClockConfig cc;
  lite_config_load_clock(cc);
  s_sntpHost = cc.sntp_hostname;
  clock.setTzOffsetMinutes(cc.tz_offset_min);
  // ... existing config load / manager seeding / mg_mgr_init / mg_bind unchanged ...
```

In `web_server_lite_loop`, after the existing `mg_mgr_poll(...)` call, add the sync driver:

```cpp
  unsigned long now = millis();
  if (s_clock && (s_clock->resyncDue(now)) &&
      (now - s_lastSntpAttemptMs) >= SNTP_RETRY_MS) {
    s_lastSntpAttemptMs = now;
    // mg_sntp_get_time opens a connection and retries internally; reply -> sntp_ev_handler.
    mg_sntp_get_time(&s_mgr, sntp_ev_handler, s_sntpHost.c_str());
  }
```

- [ ] **Step 4: Build the device env**

Run: `pio run -e openevse_lite 2>&1 | tail -3`
Expected: `[SUCCESS]` (confirms `MG_ENABLE_SNTP=1` pulls in `mg_sntp_*` and the glue compiles).

- [ ] **Step 5: Commit**

```bash
git add platformio.ini src/lite/web_server_lite.h src/lite/web_server_lite.cpp
git commit -m "feat(lite): enable Mongoose SNTP; sync LiteClock on the poll loop"
```

---

### Task 5: Emit `time` + `total_*` in `/status`; accept clock keys in `/config`

**Files:**
- Modify: `src/lite/web_server_lite.cpp` (`build_status_json`, `handle_config`)

- [ ] **Step 1: Add the time + totals fields to `build_status_json`**

In `build_status_json` (the function that fills the status JSON), after the existing energy fields, add:

```cpp
  if (s_clock && s_clock->valid()) {
    char isobuf[24];
    lite_clock_iso8601(s_clock->nowUtc(millis()), isobuf, sizeof(isobuf));
    doc["time"] = isobuf;                        // ISO-8601 UTC; omitted until first sync
  }
  if (s_totals) {
    doc["total_energy"]   = s_totals->lifetime_wh / 1000.0;  // kWh
    doc["total_day"]      = s_totals->day_wh    / 1000.0;
    doc["total_week"]     = s_totals->week_wh   / 1000.0;
    doc["total_month"]    = s_totals->month_wh  / 1000.0;
    doc["total_year"]     = s_totals->year_wh   / 1000.0;
    doc["total_switches"] = s_totals->switches;
  }
```

If the document is a `StaticJsonDocument<N>`, bump `N` by 192 bytes to fit the seven new keys (e.g. `1024` → `1216`).

- [ ] **Step 2: Accept `tz_offset_min` + `sntp_hostname` in `handle_config`**

In `handle_config`, alongside the existing `max_current_*` query-var handling, add (using the same `mg_get_http_var` pattern already in that function):

```cpp
  char tzbuf[8];
  if (mg_get_http_var(&hm->query_string, "tz_offset_min", tzbuf, sizeof(tzbuf)) > 0) {
    LiteClockConfig cc; lite_config_load_clock(cc);
    cc.tz_offset_min = atoi(tzbuf);
    lite_config_save_clock(cc);
    if (s_clock) s_clock->setTzOffsetMinutes(cc.tz_offset_min);
  }
  char hostbuf[48];
  if (mg_get_http_var(&hm->query_string, "sntp_hostname", hostbuf, sizeof(hostbuf)) > 0) {
    LiteClockConfig cc; lite_config_load_clock(cc);
    cc.sntp_hostname = hostbuf;
    lite_config_save_clock(cc);
    s_sntpHost = hostbuf;
  }
```

- [ ] **Step 3: Build the device env**

Run: `pio run -e openevse_lite 2>&1 | tail -3`
Expected: `[SUCCESS]`.

- [ ] **Step 4: Commit**

```bash
git add src/lite/web_server_lite.cpp
git commit -m "feat(lite): emit time + total_* (kWh) in /status; tz/sntp_hostname in /config"
```

---

### Task 6: Wire the clock + totals into `main_lite` (load, accrue, persist)

**Files:**
- Modify: `src/lite/main_lite.cpp`

- [ ] **Step 1: Add includes + module-static instances**

Near the existing includes/statics (after `#include "lite_evse_manager.h"`):

```cpp
#include "lite_clock.h"
#include "lite_energy_totals.h"
```

After `static LiteEvseManager s_manager(s_backend);`:

```cpp
static LiteClock        s_clock;
static LiteEnergyTotals s_totals;
static bool             s_wasCharging = false;  // for the charge->idle accrual edge
```

- [ ] **Step 2: Load totals + pass refs to the web server in `setup()`**

Replace the existing `web_server_lite_begin(s_manager);` call with:

```cpp
  lite_config_begin();
  if (!lite_config_load_totals(s_totals)) {
    energy_totals_init(s_totals);   // first boot / key absent
  }
  web_server_lite_begin(s_manager, s_clock, s_totals);
```

(Remove the now-duplicate `lite_config_begin();` line that previously preceded `web_server_lite_begin`.)

- [ ] **Step 3: Accrue + persist on the charge->idle edge in `loop()`**

Replace the body of `loop()` with:

```cpp
void loop()
{
  web_server_lite_loop();
  s_backend.loop();
  s_manager.loop();   // ticks session energy; fires its own session-complete edge

  // Mirror the manager's charge->idle edge to bank the finished session's Wh into the
  // persistent lifetime totals. Session energy freezes on stop (resets only on the next
  // rising edge), so getSessionWattHours() still holds the completed total here.
  bool charging = s_manager.isCharging();
  if (s_wasCharging && !charging) {
    energy_totals_add(s_totals, s_manager.getSessionWattHours(),
                      s_clock.nowLocal(millis()), s_clock.valid());
    lite_config_save_totals(s_totals);
  }
  s_wasCharging = charging;
}
```

- [ ] **Step 4: Build the device env**

Run: `pio run -e openevse_lite 2>&1 | tail -3`
Expected: `[SUCCESS]`.

- [ ] **Step 5: Commit**

```bash
git add src/lite/main_lite.cpp
git commit -m "feat(lite): wire clock + totals into main; accrue+persist session Wh on charge-stop"
```

---

### Task 7: Full verification — native suite, production build, bench NTP validation

**Files:** none (verification only).

- [ ] **Step 1: Run the full native suite**

Run: `pio test -e native 2>&1 | grep -E "PASSED|FAILED"`
Expected: all suites `PASSED`, including `test_lite_clock` and `test_lite_energy_totals`. No `FAILED`.

- [ ] **Step 2: Production build (no JB_DEBUG) — confirm flash/RAM still fit**

Run: `pio run -e openevse_lite 2>&1 | tail -6`
Expected: `[SUCCESS]`; note the Flash/RAM % (SNTP + the two units should add only a few KB).

- [ ] **Step 3: Flash with creds + JB_DEBUG and HW-validate NTP over WiFi (bench)**

This half is bench-validatable — NTP runs over WiFi and does not touch the (faulted) ATmega. Build + upload in ONE shell with creds exported (the established procedure), then poll `/status`:

```bash
DIR=/home/rar/oevse/openevse-juicebox-lite
SSID=$(sed -n 's/^wifi_ssid:[[:space:]]*//p' ~/secrets.yaml)
PASS=$(sed -n 's/^wifi_password:[[:space:]]*//p' ~/secrets.yaml)
export PLATFORMIO_BUILD_FLAGS="-DJB_DEBUG -DLT_LOGLEVEL=LT_LEVEL_DEBUG -D LITE_WIFI_SSID='\"$SSID\"' -D LITE_WIFI_PASS='\"$PASS\"'"
pio run -d "$DIR" -e openevse_lite
grep -aoc -F "$SSID" "$DIR/.pio/build/openevse_lite/firmware_a.bin"   # must be >= 1
pio run -d "$DIR" -e openevse_lite -t upload
```

Then, after boot+join (~15 s), poll a few times:

```bash
for i in $(seq 1 6); do curl -s --max-time 4 http://10.75.1.216/status | python3 -c "import sys,json;d=json.load(sys.stdin);print('time=',d.get('time'),' total_energy=',d.get('total_energy'),' total_day=',d.get('total_day'))"; sleep 5; done
```

Expected: `time` is absent for the first poll or two, then populates with the correct current UTC (`YYYY-MM-DDTHH:MM:SSZ`) and advances ~5 s per poll. `total_*` are `0.0` (no charging sessions on the faulted bench).

- [ ] **Step 4: Record the outcome**

Note in the final summary: NTP sync HW-validated (time populated + advancing); totals-accrual deferred to a complete (non-faulted) unit since the bench can't charge. No commit (verification only).

---

## Notes for the implementer

- **Pure units carry no `OPENEVSE_LITE` guard** (like `lite_session_energy.h`) so the native build compiles them. The integration files (`web_server_lite`, `main_lite`, `lite_config_store`) stay guarded.
- **`time` is UTC**; `tz_offset_min` shifts only the local epoch used for totals bucketing (so "day" aligns to the user's local day). This matches the spec — do not apply tz to the `time` field.
- **Flash wear:** totals persist once per completed session (infrequent) — no periodic save loop. Do not add one.
- **Don't go silent on SNTP failure:** `mg_sntp_get_time` retries internally; on failure the clock keeps free-running on `millis()` and retries at the next `SNTP_RETRY_MS`/resync window. Never block `loop()`.
