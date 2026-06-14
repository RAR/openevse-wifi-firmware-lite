# Slice 3b: `/override` Control Endpoint Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a method-routed OpenEVSE-compatible `/override` endpoint (GET/POST/DELETE/PATCH) so the firstof9/openevse Home Assistant integration can start/stop charging, set charge current, and impose session energy/time limits — routed through the existing `LiteEvseManager` seam to the JuiceBox `AL` keepalive.

**Architecture:** A pure native-tested `lite_override` unit decides limit-expiry / auto-release actions from session Wh/elapsed; `web_server_lite.cpp` holds the volatile override limit state, routes `/override` by HTTP method (JSON body parse for POST), and enforces the pure decision each loop. No persistence (overrides are volatile, matching OpenEVSE).

**Tech Stack:** C++17, Mongoose 6 (`http_message`/`mg_vcmp`/`mg_str`), ArduinoJson, doctest (native), PlatformIO (`env:native`, `env:openevse_lite`).

**Reference:** `docs/superpowers/specs/2026-06-13-lite-slice3b-override-design.md`

---

## File Structure

- **Create** `src/lite/lite_override.h` — `LiteOverrideLimits`, `LiteOverrideAction`, `lite_override_evaluate()` (pure).
- **Create** `src/lite/lite_override.cpp` — pure decision impl.
- **Create** `test/test_lite_override/test_lite_override.cpp` — doctest suite.
- **Modify** `src/lite/web_server_lite.cpp` — method-routed `/override` + loop enforcement; `#include "lite_override.h"`; bump `LITE_FW_VERSION`.
- **Modify** `platformio.ini` — add `+<lite/lite_override.cpp>` to `[env:native]` `build_src_filter`.

---

### Task 1: Pure `lite_override` decision unit (TDD, native)

**Files:**
- Create: `src/lite/lite_override.h`
- Create: `src/lite/lite_override.cpp`
- Test: `test/test_lite_override/test_lite_override.cpp`
- Modify: `platformio.ini` (`[env:native]` build_src_filter)

- [ ] **Step 1: Write the header**

Create `src/lite/lite_override.h`:

```cpp
#pragma once
#include <stdint.h>

// Session-relative override limits — a lite extension to EvseProperties (which on this
// fork has no energy/time limit fields). A limit is "set" only when its has_* flag is
// true; energy is Wh, time is seconds. Both compared against LiteSessionEnergy totals
// (session-relative, reset per plug-in) so NO wall-clock/NTP is needed.
struct LiteOverrideLimits {
  uint32_t energy_limit_wh = 0;
  uint32_t time_limit_s    = 0;
  bool     has_energy      = false;
  bool     has_time        = false;
};

enum class LiteOverrideAction : uint8_t {
  None,    // leave the override unchanged
  Stop,    // a session limit was exceeded -> caller re-asserts a sticky Disable
  Release  // an Active auto-release override hit the charge->idle edge -> caller releases
};

// Pure decision for the override-enforcement tick. See the spec for the precedence rules.
//   limits          - the active override's session limits
//   sessionWh       - LiteSessionEnergy::wattHours()
//   sessionElapsedS - LiteSessionEnergy::elapsedSecs()
//   overrideActive  - is a manual override currently claimed?
//   overrideEnabling- does the override resolve to a charging-enabling (Active) state?
//   autoRelease     - the override's auto_release flag
//   chargingFalling - true only on the tick where isCharging() went true -> false
// Precedence: a limit Stop outranks an auto-release Release.
LiteOverrideAction lite_override_evaluate(const LiteOverrideLimits &limits,
                                          uint32_t sessionWh, uint32_t sessionElapsedS,
                                          bool overrideActive, bool overrideEnabling,
                                          bool autoRelease, bool chargingFalling);
```

- [ ] **Step 2: Write the failing test**

Create `test/test_lite_override/test_lite_override.cpp`:

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "lite_override.h"

static LiteOverrideLimits energyLimit(uint32_t wh) {
  LiteOverrideLimits l; l.energy_limit_wh = wh; l.has_energy = true; return l;
}
static LiteOverrideLimits timeLimit(uint32_t s) {
  LiteOverrideLimits l; l.time_limit_s = s; l.has_time = true; return l;
}

TEST_CASE("inactive override never acts") {
  CHECK(lite_override_evaluate(energyLimit(100), 999, 0, false, true, true, true)
        == LiteOverrideAction::None);
}

TEST_CASE("no limits, no edge -> None") {
  LiteOverrideLimits none;
  CHECK(lite_override_evaluate(none, 50000, 99999, true, true, true, false)
        == LiteOverrideAction::None);
}

TEST_CASE("energy limit reached -> Stop (>= boundary)") {
  CHECK(lite_override_evaluate(energyLimit(100), 100, 0, true, true, false, false)
        == LiteOverrideAction::Stop);
  CHECK(lite_override_evaluate(energyLimit(100), 99, 0, true, true, false, false)
        == LiteOverrideAction::None);
}

TEST_CASE("time limit reached -> Stop (>= boundary)") {
  CHECK(lite_override_evaluate(timeLimit(3600), 0, 3600, true, true, false, false)
        == LiteOverrideAction::Stop);
  CHECK(lite_override_evaluate(timeLimit(3600), 0, 3599, true, true, false, false)
        == LiteOverrideAction::None);
}

TEST_CASE("Active auto-release on charge->idle edge -> Release") {
  LiteOverrideLimits none;
  CHECK(lite_override_evaluate(none, 0, 0, true, true, true, true)
        == LiteOverrideAction::Release);
  // no edge -> None
  CHECK(lite_override_evaluate(none, 0, 0, true, true, true, false)
        == LiteOverrideAction::None);
  // auto_release false -> None even on edge
  CHECK(lite_override_evaluate(none, 0, 0, true, true, false, true)
        == LiteOverrideAction::None);
  // not enabling (Disabled) + edge -> None (sticky, never released)
  CHECK(lite_override_evaluate(none, 0, 0, true, false, true, true)
        == LiteOverrideAction::None);
}

TEST_CASE("limit Stop outranks auto-release Release") {
  // limit exceeded AND Active+auto_release+edge -> Stop wins
  CHECK(lite_override_evaluate(energyLimit(100), 100, 0, true, true, true, true)
        == LiteOverrideAction::Stop);
}
```

- [ ] **Step 3: Run the test, verify it fails to link/compile**

Run: `pio test -e native -f test_lite_override`
Expected: FAIL — `lite_override_evaluate` undefined.

- [ ] **Step 4: Write the implementation**

Create `src/lite/lite_override.cpp`:

```cpp
#include "lite_override.h"

LiteOverrideAction lite_override_evaluate(const LiteOverrideLimits &limits,
                                          uint32_t sessionWh, uint32_t sessionElapsedS,
                                          bool overrideActive, bool overrideEnabling,
                                          bool autoRelease, bool chargingFalling) {
  if (!overrideActive) return LiteOverrideAction::None;

  // Limit Stop has precedence over auto-release.
  if ((limits.has_energy && sessionWh >= limits.energy_limit_wh) ||
      (limits.has_time   && sessionElapsedS >= limits.time_limit_s)) {
    return LiteOverrideAction::Stop;
  }

  // One-shot Active auto-release: release when charging falls to idle. Disabled / expired
  // overrides are sticky (overrideEnabling == false) and never auto-released here.
  if (overrideEnabling && autoRelease && chargingFalling) {
    return LiteOverrideAction::Release;
  }

  return LiteOverrideAction::None;
}
```

- [ ] **Step 5: Add the .cpp to the native build filter**

In `platformio.ini`, append `+<lite/lite_override.cpp>` to the `[env:native]` `build_src_filter` line (after `+<lite/lite_energy_totals.cpp>`).

- [ ] **Step 6: Run the test, verify it passes**

Run: `pio test -e native -f test_lite_override`
Expected: PASS (all cases).

- [ ] **Step 7: Commit**

```bash
git add src/lite/lite_override.h src/lite/lite_override.cpp test/test_lite_override/ platformio.ini
git commit -m "feat(lite): pure override limit/auto-release decision unit (native-tested)"
```

---

### Task 2: Method-routed `/override` HTTP handler

**Files:**
- Modify: `src/lite/web_server_lite.cpp`

Replace the query-param `/override` stub with a method-routed handler (GET/POST/DELETE/PATCH), JSON body parsing, and file-static override state. Enforcement (loop) is Task 3.

- [ ] **Step 1: Add the include**

In `src/lite/web_server_lite.cpp`, add after `#include "lite_charge_policy.h"`:

```cpp
#include "lite_override.h"
```

- [ ] **Step 2: Add file-static override state + helpers**

Immediately after the existing `static LiteEvseConfig s_cfg = ...;` declaration (~line 50), add:

```cpp
// ---- /override (Slice 3b) volatile state (not persisted; resets on reboot) -------------
static LiteOverrideLimits s_ovrLimits;            // limits of the active override
static bool               s_ovrExpired  = false;  // a session limit fired -> sticky Disable
static bool               s_ovrEnabling = false;  // override resolves to Active (charging)
static bool               s_wasCharging = false;  // tracks the charge->idle falling edge

static const char *override_state_str(EvseState s) {
  switch (s) {
    case EvseState::Active:   return "active";
    case EvseState::Disabled: return "disabled";
    default:                  return nullptr;       // None -> omit
  }
}

// Claim a parsed override; capture its limits + enabling flag; clear the expired latch.
static void override_apply(EvseProperties &props, const LiteOverrideLimits &lim) {
  s_ovrLimits   = lim;
  s_ovrExpired  = false;
  s_ovrEnabling = (props.getState() == EvseState::Active);
  manual.claim(props);
}

static void override_clear() {
  s_ovrLimits   = LiteOverrideLimits();
  s_ovrExpired  = false;
  s_ovrEnabling = false;
  manual.release();
}

// Parse a JSON override body into props + limits. False on JSON parse error.
static bool override_parse(const char *body, size_t len,
                           EvseProperties &props, LiteOverrideLimits &lim) {
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, body, len) != DeserializationError::Ok) return false;
  if (doc.containsKey("state")) {
    const char *s = doc["state"];
    if      (s && !strcmp(s, "active"))   props.setState(EvseState::Active);
    else if (s && !strcmp(s, "disabled")) props.setState(EvseState::Disabled);
    else if (s && !strcmp(s, "clear"))    props.setState(EvseState::None);
  }
  if (doc.containsKey("charge_current")) props.setChargeCurrent((uint32_t)doc["charge_current"]);
  if (doc.containsKey("max_current"))    props.setMaxCurrent((uint32_t)doc["max_current"]);
  if (doc.containsKey("auto_release"))   props.setAutoRelease((bool)doc["auto_release"]);
  if (doc.containsKey("energy_limit")) {
    lim.energy_limit_wh = (uint32_t)doc["energy_limit"]; lim.has_energy = true;
  }
  if (doc.containsKey("time_limit")) {
    lim.time_limit_s = (uint32_t)doc["time_limit"]; lim.has_time = true;
  }
  return true;
}

// Serialize the active override (or {}) into `out`.
static void override_get_json(String &out) {
  StaticJsonDocument<192> doc;
  if (manual.isActive()) {
    EvseProperties props;
    manual.getProperties(props);
    const char *st = override_state_str(props.getState());
    if (st) doc["state"] = st;
    if (props.hasChargeCurrent()) doc["charge_current"] = props.getChargeCurrent();
    if (props.hasMaxCurrent())    doc["max_current"]    = props.getMaxCurrent();
    doc["auto_release"] = props.isAutoRelease();
    if (s_ovrLimits.has_energy) doc["energy_limit"] = s_ovrLimits.energy_limit_wh;
    if (s_ovrLimits.has_time)   doc["time_limit"]   = s_ovrLimits.time_limit_s;
    doc["expired"] = s_ovrExpired;
  }
  serializeJson(doc, out);
}

static void handle_override(struct mg_connection *nc, struct http_message *hm) {
  int code = 200;
  String body;

  // Legacy bodyless convenience (same rationale as /config): ?state=active|disabled|
  // release|clear short-circuits to a claim/release regardless of method.
  char qstate[12];
  if (mg_get_http_var(&hm->query_string, "state", qstate, sizeof(qstate)) > 0) {
    if      (!strcmp(qstate, "active"))   { EvseProperties p(EvseState::Active);   LiteOverrideLimits l; override_apply(p, l); }
    else if (!strcmp(qstate, "disabled")) { EvseProperties p(EvseState::Disabled); LiteOverrideLimits l; override_apply(p, l); }
    else if (!strcmp(qstate, "release") || !strcmp(qstate, "clear")) { override_clear(); }
    override_get_json(body);
  } else if (mg_vcmp(&hm->method, "POST") == 0) {
    EvseProperties props;
    LiteOverrideLimits lim;
    if (override_parse(hm->body.p, hm->body.len, props, lim)) {
      override_apply(props, lim);
      code = 201; body = "{\"msg\":\"Created\"}";
    } else {
      code = 400; body = "{\"msg\":\"Failed to parse JSON\"}";
    }
  } else if (mg_vcmp(&hm->method, "DELETE") == 0) {
    override_clear();
    body = "{\"msg\":\"Deleted\"}";
  } else if (mg_vcmp(&hm->method, "PATCH") == 0) {
    manual.toggle();
    s_ovrLimits = LiteOverrideLimits(); s_ovrExpired = false;
    EvseProperties tp; s_ovrEnabling = manual.getProperties(tp) && tp.getState() == EvseState::Active;
    body = "{\"msg\":\"Updated\"}";
  } else {
    override_get_json(body);   // GET
  }

  mg_send_head(nc, code, body.length(), "Content-Type: application/json");
  mg_printf(nc, "%s", body.c_str());
}
```

- [ ] **Step 3: Route `/override` to the handler**

In `ev_handler`, replace the entire existing `} else if (mg_vcmp(&hm->uri, "/override") == 0) { ... }` block (the query-param stub, ~lines 229-247) with:

```cpp
  } else if (mg_vcmp(&hm->uri, "/override") == 0) {
    handle_override(nc, hm);
```

- [ ] **Step 4: Bump the firmware version**

Change `#define LITE_FW_VERSION "lite-3a"` to `"lite-3b"`.

- [ ] **Step 5: Build the device env**

Run: `pio run -e openevse_lite`
Expected: SUCCESS (links; flash % printed). If `hm->body`/`hm->method` field names differ in this Mongoose build, check `mongoose.h`'s `struct http_message` and adjust (`mg_str body; mg_str method;` are standard for Mongoose 6).

- [ ] **Step 6: Commit**

```bash
git add src/lite/web_server_lite.cpp
git commit -m "feat(lite): method-routed /override (GET/POST/DELETE/PATCH) with JSON body + limits"
```

---

### Task 3: Loop enforcement of limits + auto-release

**Files:**
- Modify: `src/lite/web_server_lite.cpp` (`web_server_lite_loop`)

- [ ] **Step 1: Add the enforcement tick**

In `web_server_lite_loop()`, immediately after the `mg_mgr_poll(&s_mgr, 0);` line and before the SNTP block, add:

```cpp
  // Slice 3b: enforce override session limits + auto-release. Pure decision in
  // lite_override_evaluate; this is the thin wiring to the manager seam.
  if (s_mgr_ctrl) {
    bool charging    = s_mgr_ctrl->isCharging();
    bool fallingEdge = (s_wasCharging && !charging);
    s_wasCharging    = charging;
    if (manual.isActive() && !s_ovrExpired) {
      EvseProperties cur; bool haveCur = manual.getProperties(cur);
      LiteOverrideAction act = lite_override_evaluate(
          s_ovrLimits,
          s_mgr_ctrl->getSessionWattHours(),
          s_mgr_ctrl->getSessionElapsed(),
          true, s_ovrEnabling, haveCur && cur.isAutoRelease(), fallingEdge);
      if (act == LiteOverrideAction::Stop) {
        EvseProperties p(EvseState::Disabled);
        manual.claim(p);
        s_ovrExpired = true; s_ovrEnabling = false;   // sticky: stays stopped until DELETE
      } else if (act == LiteOverrideAction::Release) {
        override_clear();
      }
    }
  }
```

- [ ] **Step 2: Build the device env**

Run: `pio run -e openevse_lite`
Expected: SUCCESS.

- [ ] **Step 3: Commit**

```bash
git add src/lite/web_server_lite.cpp
git commit -m "feat(lite): enforce override energy/time limits + auto-release in web loop"
```

---

### Task 4: Full native suite + production build verification

**Files:** none (verification only)

- [ ] **Step 1: Run the full native suite**

Run: `pio test -e native`
Expected: ALL suites PASS (existing + `test_lite_override`).

- [ ] **Step 2: Production device build**

Run: `pio run -e openevse_lite`
Expected: SUCCESS; note the flash % (was ~23.8% pre-slice; expect a small increase).

- [ ] **Step 3: Verify no regression in existing endpoints**

Confirm `/status` and `/config` handlers are unchanged (the only `ev_handler` edit is the `/override` route). Grep: `grep -n '"/status"\|"/config"\|"/override"' src/lite/web_server_lite.cpp` — three routes intact.

- [ ] **Step 4: Final commit (if any cleanup)**

No code change expected; if clean, nothing to commit. Slice 3b is code-complete + native-tested. On-device charge-control validation is DEFERRED (bench unit hard-faults on GFI — see spec).
```
