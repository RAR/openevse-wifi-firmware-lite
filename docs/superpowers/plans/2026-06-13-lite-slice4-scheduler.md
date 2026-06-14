# Slice 4: Time-of-Day Charging Scheduler Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A persistent weekly charging schedule — OpenEVSE-shaped `/schedule` events evaluated against the local wall-clock to claim charging-enabled/disabled through the manager seam, below a manual override.

**Architecture:** A pure native-tested `lite_schedule` unit (week-relative "most-recently-fired" eval + time/day parsing + table upsert/remove); a FlashDB blob for persistence; OpenEVSE-shaped `/schedule` CRUD routes; a loop that claims the `Schedule` client (priority 100) when the clock is valid.

**Tech Stack:** C++17, Mongoose 6, ArduinoJson, FlashDB, doctest (native), PlatformIO.

**Reference:** `docs/superpowers/specs/2026-06-13-lite-slice4-scheduler-design.md`

---

## File Structure

- **Create** `src/lite/lite_schedule.h` / `.cpp` — pure schedule core.
- **Create** `test/test_lite_schedule/test_lite_schedule.cpp` — doctest suite.
- **Modify** `src/lite/lite_config_store.{h,cpp}` — schedule blob persistence.
- **Modify** `src/lite/web_server_lite.cpp` — `/schedule` routes + loop eval/claim + `schedule_version`; bump `LITE_FW_VERSION`.
- **Modify** `platformio.ini` — `+<lite/lite_schedule.cpp>` on `[env:native]`.

---

### Task 1: Pure `lite_schedule` unit (TDD, native)

**Files:**
- Create: `src/lite/lite_schedule.h`, `src/lite/lite_schedule.cpp`
- Test: `test/test_lite_schedule/test_lite_schedule.cpp`
- Modify: `platformio.ini` (`[env:native]` build_src_filter)

- [ ] **Step 1: Write the header** — create `src/lite/lite_schedule.h`:

```cpp
#pragma once
#include <stdint.h>
#include <stddef.h>

#define LITE_SCHEDULE_MAX_EVENTS 16

// One weekly schedule event. state uses EvseState values (1=Active, 2=Disabled); id 0 is
// reserved "empty". Fixed 12-byte layout for the persisted blob.
struct LiteScheduleEvent {
  uint32_t id;          // client-assigned, nonzero
  uint8_t  day_mask;    // bit0=Sun .. bit6=Sat
  uint8_t  state;       // 1=Active, 2=Disabled
  uint8_t  pad[2];      // zero; keeps the struct 4-byte aligned and blob-stable
  uint32_t sec_of_day;  // 0..86399 fire time (local)
};

struct LiteSchedule {
  LiteScheduleEvent events[LITE_SCHEDULE_MAX_EVENTS];
  uint32_t count;       // populated slots [0..MAX]
};

// Most-recently-fired event's state as of (dayOfWeek 0=Sun..6=Sat, secOfDay 0..86399).
// Returns 0 (EvseState::None) when no event applies. Pure.
uint8_t lite_schedule_active_state(const LiteSchedule &s, int dayOfWeek, uint32_t secOfDay);

// Upsert by id (replace same-id, else append). False if full and id not present. Pure.
bool lite_schedule_upsert(LiteSchedule &s, const LiteScheduleEvent &e);
// Remove by id. False if absent. Pure. Compacts the array (order not preserved).
bool lite_schedule_remove(LiteSchedule &s, uint32_t id);

// "HH:MM" or "HH:MM:SS" -> seconds of day. False on malformed/out-of-range. Pure.
bool lite_schedule_parse_time(const char *hhmmss, uint32_t &secOfDay);
// secOfDay -> "HH:MM:SS" (cap >= 9). Pure.
void lite_schedule_format_time(uint32_t secOfDay, char *buf, size_t cap);
// lowercase day name (e.g. "monday") -> bit index 0..6 (Sun=0); -1 unknown. Pure.
int  lite_schedule_day_index(const char *name);
// bit index 0..6 -> lowercase day name (static string); nullptr if out of range. Pure.
const char *lite_schedule_day_name(int index);
```

- [ ] **Step 2: Write the failing test** — create `test/test_lite_schedule/test_lite_schedule.cpp`:

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../src/lite/lite_schedule.h"
#include <string.h>

static LiteScheduleEvent ev(uint32_t id, uint8_t mask, uint8_t state, uint32_t sod) {
  LiteScheduleEvent e; memset(&e, 0, sizeof(e));
  e.id = id; e.day_mask = mask; e.state = state; e.sec_of_day = sod; return e;
}
#define DAY(i) (uint8_t)(1u << (i))   // 0=Sun..6=Sat

TEST_CASE("parse_time") {
  uint32_t s = 0;
  CHECK(lite_schedule_parse_time("23:00", s));    CHECK(s == 82800u);
  CHECK(lite_schedule_parse_time("06:30:15", s)); CHECK(s == 23415u);
  CHECK(lite_schedule_parse_time("00:00:00", s)); CHECK(s == 0u);
  CHECK_FALSE(lite_schedule_parse_time("24:00", s));
  CHECK_FALSE(lite_schedule_parse_time("12:60", s));
  CHECK_FALSE(lite_schedule_parse_time("9x", s));
  CHECK_FALSE(lite_schedule_parse_time("", s));
}

TEST_CASE("format_time") {
  char b[16];
  lite_schedule_format_time(82800, b, sizeof(b)); CHECK(strcmp(b, "23:00:00") == 0);
  lite_schedule_format_time(0, b, sizeof(b));     CHECK(strcmp(b, "00:00:00") == 0);
  lite_schedule_format_time(23415, b, sizeof(b)); CHECK(strcmp(b, "06:30:15") == 0);
}

TEST_CASE("day_index/day_name round trip") {
  CHECK(lite_schedule_day_index("sunday") == 0);
  CHECK(lite_schedule_day_index("saturday") == 6);
  CHECK(lite_schedule_day_index("monday") == 1);
  CHECK(lite_schedule_day_index("funday") == -1);
  for (int i = 0; i < 7; i++) CHECK(lite_schedule_day_index(lite_schedule_day_name(i)) == i);
  CHECK(lite_schedule_day_name(7) == nullptr);
}

TEST_CASE("upsert appends and replaces") {
  LiteSchedule s; memset(&s, 0, sizeof(s));
  CHECK(lite_schedule_upsert(s, ev(1, DAY(1), 1, 100))); CHECK(s.count == 1);
  CHECK(lite_schedule_upsert(s, ev(2, DAY(2), 2, 200))); CHECK(s.count == 2);
  // replace id 1 -> count unchanged, fields updated
  CHECK(lite_schedule_upsert(s, ev(1, DAY(3), 2, 300))); CHECK(s.count == 2);
  CHECK(lite_schedule_active_state(s, 3, 400) == 2);  // Wed 00:06:40, id1 fired at 300
}

TEST_CASE("upsert full table") {
  LiteSchedule s; memset(&s, 0, sizeof(s));
  for (uint32_t i = 1; i <= LITE_SCHEDULE_MAX_EVENTS; i++)
    CHECK(lite_schedule_upsert(s, ev(i, DAY(0), 1, i)));
  CHECK(s.count == LITE_SCHEDULE_MAX_EVENTS);
  CHECK_FALSE(lite_schedule_upsert(s, ev(999, DAY(0), 1, 1))); // new id, full
  CHECK(lite_schedule_upsert(s, ev(1, DAY(0), 2, 5)));         // replace existing, ok when full
}

TEST_CASE("remove compacts") {
  LiteSchedule s; memset(&s, 0, sizeof(s));
  lite_schedule_upsert(s, ev(1, DAY(1), 1, 100));
  lite_schedule_upsert(s, ev(2, DAY(2), 2, 200));
  lite_schedule_upsert(s, ev(3, DAY(3), 1, 300));
  CHECK(lite_schedule_remove(s, 2)); CHECK(s.count == 2);
  CHECK_FALSE(lite_schedule_remove(s, 2));   // already gone
  CHECK_FALSE(lite_schedule_remove(s, 99));  // never existed
}

TEST_CASE("active_state empty -> 0") {
  LiteSchedule s; memset(&s, 0, sizeof(s));
  CHECK(lite_schedule_active_state(s, 3, 50000) == 0);
}

TEST_CASE("active_state same-day two events") {
  LiteSchedule s; memset(&s, 0, sizeof(s));
  lite_schedule_upsert(s, ev(1, DAY(3), 2, 6 * 3600));   // Wed 06:00 Disabled
  lite_schedule_upsert(s, ev(2, DAY(3), 1, 23 * 3600));  // Wed 23:00 Active
  CHECK(lite_schedule_active_state(s, 3, 12 * 3600) == 2); // noon -> Disabled
  CHECK(lite_schedule_active_state(s, 3, 23 * 3600 + 1800) == 1); // 23:30 -> Active
}

TEST_CASE("active_state wraps across week") {
  LiteSchedule s; memset(&s, 0, sizeof(s));
  lite_schedule_upsert(s, ev(1, DAY(6), 1, 23 * 3600));  // Saturday 23:00 Active only
  CHECK(lite_schedule_active_state(s, 0, 5 * 3600) == 1); // Sunday 05:00 -> still Active (6h ago)
}

TEST_CASE("active_state multi-day mask") {
  LiteSchedule s; memset(&s, 0, sizeof(s));
  lite_schedule_upsert(s, ev(1, (uint8_t)(DAY(1) | DAY(3) | DAY(5)), 1, 8 * 3600)); // MWF 08:00
  CHECK(lite_schedule_active_state(s, 3, 9 * 3600) == 1);  // Wed 09:00 -> Active
}
```

- [ ] **Step 3: Run, verify it fails** — `pio test -e native -f test_lite_schedule` → FAIL (undefined refs).

- [ ] **Step 4: Implement** — create `src/lite/lite_schedule.cpp`:

```cpp
#include "lite_schedule.h"
#include <stdio.h>
#include <string.h>

static_assert(sizeof(LiteScheduleEvent) == 12, "LiteScheduleEvent must be 12 bytes (blob-stable)");
static_assert(sizeof(LiteSchedule) == 12 * LITE_SCHEDULE_MAX_EVENTS + 4,
              "LiteSchedule blob size changed");

uint8_t lite_schedule_active_state(const LiteSchedule &s, int dayOfWeek, uint32_t secOfDay) {
  const uint32_t WEEK = 7u * 86400u;
  uint32_t now = (uint32_t)dayOfWeek * 86400u + secOfDay;
  uint32_t bestAgo = WEEK; uint8_t bestState = 0;
  for (uint32_t i = 0; i < s.count && i < LITE_SCHEDULE_MAX_EVENTS; i++) {
    const LiteScheduleEvent &e = s.events[i];
    if (e.id == 0) continue;
    for (int d = 0; d < 7; d++) {
      if (!(e.day_mask & (1u << d))) continue;
      uint32_t fire = (uint32_t)d * 86400u + e.sec_of_day;
      uint32_t ago  = (now + WEEK - fire) % WEEK;
      if (ago < bestAgo) { bestAgo = ago; bestState = e.state; }
    }
  }
  return bestState;
}

bool lite_schedule_upsert(LiteSchedule &s, const LiteScheduleEvent &e) {
  for (uint32_t i = 0; i < s.count && i < LITE_SCHEDULE_MAX_EVENTS; i++) {
    if (s.events[i].id == e.id) { s.events[i] = e; return true; }
  }
  if (s.count >= LITE_SCHEDULE_MAX_EVENTS) return false;
  s.events[s.count++] = e;
  return true;
}

bool lite_schedule_remove(LiteSchedule &s, uint32_t id) {
  for (uint32_t i = 0; i < s.count && i < LITE_SCHEDULE_MAX_EVENTS; i++) {
    if (s.events[i].id == id) {
      s.events[i] = s.events[s.count - 1];   // compact: move last into the gap
      memset(&s.events[s.count - 1], 0, sizeof(LiteScheduleEvent));
      s.count--;
      return true;
    }
  }
  return false;
}

bool lite_schedule_parse_time(const char *t, uint32_t &secOfDay) {
  if (!t) return false;
  int h = -1, m = -1, sec = 0;
  int n = sscanf(t, "%d:%d:%d", &h, &m, &sec);
  if (n < 2) return false;
  if (h < 0 || h > 23 || m < 0 || m > 59 || sec < 0 || sec > 59) return false;
  secOfDay = (uint32_t)h * 3600u + (uint32_t)m * 60u + (uint32_t)sec;
  return true;
}

void lite_schedule_format_time(uint32_t secOfDay, char *buf, size_t cap) {
  if (!buf || cap == 0) return;
  secOfDay %= 86400u;
  unsigned h = secOfDay / 3600u, m = (secOfDay % 3600u) / 60u, s = secOfDay % 60u;
  snprintf(buf, cap, "%02u:%02u:%02u", h, m, s);
}

static const char *const kDayNames[7] = {
  "sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday"
};

int lite_schedule_day_index(const char *name) {
  if (!name) return -1;
  for (int i = 0; i < 7; i++) if (strcmp(name, kDayNames[i]) == 0) return i;
  return -1;
}

const char *lite_schedule_day_name(int index) {
  return (index >= 0 && index < 7) ? kDayNames[index] : nullptr;
}
```

NOTE: `sscanf("%d:%d:%d")` accepts "06:30" (n==2, sec stays 0) and "06:30:15" (n==3). It
also accepts a trailing garbage like "12:30x" as n==2 — acceptable (lenient parse); the
range checks reject the clearly-invalid cases the tests require.

- [ ] **Step 5: Add to native build filter** — in `platformio.ini`, append `+<lite/lite_schedule.cpp>` to the `[env:native]` `build_src_filter` line.

- [ ] **Step 6: Run, verify pass** — `pio test -e native -f test_lite_schedule` → PASS.

- [ ] **Step 7: Commit**

```bash
git add src/lite/lite_schedule.h src/lite/lite_schedule.cpp test/test_lite_schedule/ platformio.ini
git commit -m "feat(lite): pure weekly schedule eval + time/day helpers (native-tested)"
```

---

### Task 2: Schedule blob persistence

**Files:**
- Modify: `src/lite/lite_config_store.h`, `src/lite/lite_config_store.cpp`

- [ ] **Step 1: Declare the API** — in `src/lite/lite_config_store.h`, add `#include "lite_schedule.h"` near the other includes, and after the totals declarations add:

```cpp
bool lite_config_load_schedule(LiteSchedule &out);     // false if key absent (caller zero-inits)
bool lite_config_save_schedule(const LiteSchedule &in);
```

- [ ] **Step 2: Implement** — in `src/lite/lite_config_store.cpp`, mirror the totals blob functions (add `#include "lite_schedule.h"` if not pulled in transitively):

```cpp
bool lite_config_load_schedule(LiteSchedule &out)
{
  if (!s_ready) return false;
  struct fdb_blob blob;
  fdb_kv_get_blob(&s_kvdb, "sched", fdb_blob_make(&blob, &out, sizeof(out)));
  return blob.saved.len == sizeof(out);
}

bool lite_config_save_schedule(const LiteSchedule &in)
{
  if (!s_ready) return false;
  struct fdb_blob blob;
  fdb_err_t err = fdb_kv_set_blob(&s_kvdb, "sched",
      fdb_blob_make(&blob, &in, sizeof(in)));
  return err == FDB_NO_ERR;
}
```

(Match the exact field/return style of the existing `lite_config_save_totals` — if it
differs, copy that function's idiom verbatim, changing only the key string and type.)

- [ ] **Step 3: Build device env** — `pio run -e openevse_lite` → SUCCESS.

- [ ] **Step 4: Commit**

```bash
git add src/lite/lite_config_store.h src/lite/lite_config_store.cpp
git commit -m "feat(lite): persist weekly schedule as a FlashDB blob"
```

---

### Task 3: `/schedule` CRUD routes

**Files:**
- Modify: `src/lite/web_server_lite.cpp`

- [ ] **Step 1: Include + state** — add `#include "lite_schedule.h"` after `#include "lite_override.h"`. After the override statics (`s_wasCharging`), add:

```cpp
// ---- /schedule (Slice 4) -------------------------------------------------------------
static LiteSchedule s_schedule;            // persisted weekly schedule
static uint32_t     s_scheduleVersion = 0; // bumped on every mutation (exposed in /status)
static uint8_t      s_lastSchedState  = 0; // last schedule-resolved state applied (0/1/2)

static uint8_t sched_state_from_str(const char *s) {
  if (s && !strcmp(s, "active"))   return 1;
  if (s && !strcmp(s, "disabled")) return 2;
  return 0;
}
static const char *sched_state_str(uint8_t st) { return st == 1 ? "active" : "disabled"; }

// Serialize the whole schedule as a JSON array of {id,state,time,days}.
static void schedule_get_json(String &out) {
  // 16 events x {id,state,time,days[<=7]} -> ~2.1 KB worst case; size generously.
  StaticJsonDocument<2560> doc;
  JsonArray arr = doc.to<JsonArray>();
  for (uint32_t i = 0; i < s_schedule.count && i < LITE_SCHEDULE_MAX_EVENTS; i++) {
    const LiteScheduleEvent &e = s_schedule.events[i];
    JsonObject o = arr.createNestedObject();
    o["id"] = e.id;
    o["state"] = sched_state_str(e.state);
    char tb[12]; lite_schedule_format_time(e.sec_of_day, tb, sizeof(tb));
    o["time"] = tb;
    JsonArray days = o.createNestedArray("days");
    for (int d = 0; d < 7; d++) if (e.day_mask & (1u << d)) days.add(lite_schedule_day_name(d));
  }
  serializeJson(doc, out);
}

// Parse one event from a JSON body. Returns false (and leaves *code) on validation error.
static bool schedule_parse(const char *body, size_t len, LiteScheduleEvent &e, int &code) {
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, body, len) != DeserializationError::Ok) { code = 400; return false; }
  if (!doc.containsKey("state") || !doc.containsKey("time") || !doc.containsKey("days")) {
    code = 400; return false;
  }
  memset(&e, 0, sizeof(e));
  uint8_t st = sched_state_from_str(doc["state"]);
  if (st == 0) { code = 400; return false; }
  e.state = st;
  uint32_t sod;
  if (!lite_schedule_parse_time(doc["time"], sod)) { code = 400; return false; }
  e.sec_of_day = sod;
  uint8_t mask = 0;
  for (JsonVariant v : doc["days"].as<JsonArray>()) {
    int di = lite_schedule_day_index(v.as<const char *>());
    if (di < 0) { code = 400; return false; }
    mask |= (uint8_t)(1u << di);
  }
  if (mask == 0) { code = 400; return false; }
  e.day_mask = mask;
  // id: client-provided, else auto-assign max+1 (1 when empty).
  if (doc.containsKey("id") && (uint32_t)doc["id"] != 0) {
    e.id = (uint32_t)doc["id"];
  } else {
    uint32_t mx = 0;
    for (uint32_t i = 0; i < s_schedule.count && i < LITE_SCHEDULE_MAX_EVENTS; i++)
      if (s_schedule.events[i].id > mx) mx = s_schedule.events[i].id;
    e.id = mx + 1;
  }
  return true;
}

// Parse a trailing /schedule/<id> id from the URI (0 if none).
static uint32_t schedule_id_from_uri(struct http_message *hm) {
  const char *pfx = "/schedule/";
  size_t pl = strlen(pfx);
  if (hm->uri.len > pl && memcmp(hm->uri.p, pfx, pl) == 0) {
    return (uint32_t)strtoul(hm->uri.p + pl, NULL, 10);
  }
  char idbuf[12];
  if (mg_get_http_var(&hm->query_string, "id", idbuf, sizeof(idbuf)) > 0)
    return (uint32_t)strtoul(idbuf, NULL, 10);
  return 0;
}

static void handle_schedule(struct mg_connection *nc, struct http_message *hm) {
  int code = 200;
  String body;
  if (mg_vcmp(&hm->method, "POST") == 0) {
    LiteScheduleEvent e;
    if (schedule_parse(hm->body.p, hm->body.len, e, code)) {
      if (lite_schedule_upsert(s_schedule, e)) {
        bool saved = lite_config_save_schedule(s_schedule);
        s_scheduleVersion++;
        code = saved ? 201 : 503;
        StaticJsonDocument<64> r; r["id"] = e.id;
        serializeJson(r, body);
      } else {
        code = 507; body = "{\"msg\":\"Schedule full\"}";
      }
    } else {
      body = "{\"msg\":\"Bad schedule event\"}";
    }
  } else if (mg_vcmp(&hm->method, "DELETE") == 0) {
    uint32_t id = schedule_id_from_uri(hm);
    if (id != 0 && lite_schedule_remove(s_schedule, id)) {
      lite_config_save_schedule(s_schedule);
      s_scheduleVersion++;
      body = "{\"msg\":\"Deleted\"}";
    } else {
      code = 404; body = "{\"msg\":\"Not found\"}";
    }
  } else {
    schedule_get_json(body);   // GET
  }
  mg_send_head(nc, code, body.length(), "Content-Type: application/json");
  mg_printf(nc, "%s", body.c_str());
}
```

- [ ] **Step 2: Route it** — in `ev_handler`, after the `/override` branch, add:

```cpp
  } else if ((hm->uri.len == 9  && memcmp(hm->uri.p, "/schedule", 9)  == 0) ||
             (hm->uri.len > 10 && memcmp(hm->uri.p, "/schedule/", 10) == 0)) {
    handle_schedule(nc, hm);
```

- [ ] **Step 3: Load at begin + version in /status** — in `web_server_lite_begin`, after the clock config load, add:

```cpp
  if (!lite_config_load_schedule(s_schedule)) {
    memset(&s_schedule, 0, sizeof(s_schedule));
  }
```

In `build_status_json`, in the identity/system section (near `doc["uptime"]`), add:

```cpp
  doc["schedule_version"] = s_scheduleVersion;
```

- [ ] **Step 4: Build device env** — `pio run -e openevse_lite` → SUCCESS.

- [ ] **Step 5: Commit**

```bash
git add src/lite/web_server_lite.cpp
git commit -m "feat(lite): /schedule CRUD (GET/POST/DELETE) + schedule_version in /status"
```

---

### Task 4: Loop eval + Schedule claim

**Files:**
- Modify: `src/lite/web_server_lite.cpp` (`web_server_lite_loop`)

- [ ] **Step 1: Add the eval/claim tick** — in `web_server_lite_loop()`, AFTER the Slice-3b override-enforcement block and BEFORE the SNTP block, add:

```cpp
  // Slice 4: time-of-day schedule. Resolve the active scheduled state from the local
  // wall-clock and claim it via the Schedule client (priority below a manual override).
  // Released while the clock is unsynced (no time basis). Re-claim only on change.
  if (s_mgr_ctrl && s_clock) {
    uint8_t st = 0;
    if (s_clock->valid()) {
      uint32_t local = s_clock->nowLocal(millis());
      int dow = (int)(((local / 86400u) + 4u) % 7u);  // 1970-01-01 = Thursday; Sun=0
      uint32_t sod = local % 86400u;
      st = lite_schedule_active_state(s_schedule, dow, sod);
    }
    if (st != s_lastSchedState) {
      if (st == 0) {
        s_mgr_ctrl->release(EvseClient_OpenEVSE_Schedule);
      } else {
        EvseProperties p(st == 1 ? EvseState::Active : EvseState::Disabled);
        s_mgr_ctrl->claim(EvseClient_OpenEVSE_Schedule, EvseManager_Priority_Timer, p);
      }
      s_lastSchedState = st;
    }
  }
```

- [ ] **Step 2: Bump version** — change `#define LITE_FW_VERSION "lite-3b"` to `"lite-4"`.

- [ ] **Step 3: Build device env** — `pio run -e openevse_lite` → SUCCESS. Note the flash %.

- [ ] **Step 4: Commit**

```bash
git add src/lite/web_server_lite.cpp
git commit -m "feat(lite): evaluate schedule each loop + claim Schedule client (below manual)"
```

---

### Task 5: Full native suite + production build verification

**Files:** none (verification only)

- [ ] **Step 1: Full native suite** — `pio test -e native` → ALL PASS (incl. `test_lite_schedule`).

- [ ] **Step 2: Production build** — `pio run -e openevse_lite` → SUCCESS; record flash % (was 24.0% after Slice 3b).

- [ ] **Step 3: Route sanity** — `grep -n '"/status"\|"/config"\|"/override"\|"/schedule"' src/lite/web_server_lite.cpp` — confirm `/status`, `/config`, `/override` intact and the new `/schedule` route present.

- [ ] **Step 4:** No code change expected. Slice 4 is code-complete + native-tested; on-device charge-effect validation DEFERRED (bench hard-faults on GFI). The `/schedule` CRUD + `schedule_version` + clock-driven `/status` state transitions are bench-checkable over WiFi.
