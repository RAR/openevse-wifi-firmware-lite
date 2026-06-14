# Slice 3a: OpenEVSE `/status` Compatibility — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `GET /status` on the JuiceBox-lite firmware emit the OpenEVSE key set the firstof9/openevse Home Assistant integration consumes, including host-side session energy.

**Architecture:** A new pure `LiteSessionEnergy` accumulator (native-testable, no Arduino) integrates instantaneous power over time. A new pure `lite_openevse_compat` unit maps the canonical EVSE state onto OpenEVSE's `state` int + `status` string. `LiteEvseManager` owns the accumulator, gains a `loop()` that ticks it from `backend.getPower()`/`isCharging()`, and exposes new getters. `web_server_lite.cpp`'s `build_status_json` assembles the full OpenEVSE response from manager getters + the two pure units.

**Tech Stack:** C++11, doctest (native host tests via `pio test -e native`), ArduinoJson, Mongoose (lite), LibreTiny/EFM32 target build (`OPENEVSE_LITE` + `LITE_EVSE_BACKEND_JUICEBOX`).

---

## File Structure

- **Create** `src/lite/lite_session_energy.h` / `.cpp` — pure session-energy accumulator. One job: integrate power → watt-seconds/Wh + track elapsed, reset on charging rising edge.
- **Create** `src/lite/lite_openevse_compat.h` / `.cpp` — pure mapping: canonical state → OpenEVSE `state` int + `status` string.
- **Create** `test/test_lite_session_energy/test_lite_session_energy.cpp` — doctest suite.
- **Create** `test/test_lite_openevse_compat/test_lite_openevse_compat.cpp` — doctest suite.
- **Modify** `src/lite/lite_evse_manager.h` / `.cpp` — add `loop()`, `LiteSessionEnergy` member, and getters: `getPower()`, `getDeviceState()`, `getSessionWattSeconds()`, `getSessionWattHours()`, `getSessionElapsed()`.
- **Modify** `src/lite/main_lite.cpp` — call `s_manager.loop()` in `loop()`.
- **Modify** `src/lite/web_server_lite.cpp` — expand `build_status_json` to the full OpenEVSE key set.

**Note on `fault_text` (spec reconciliation):** The spec mentions a `fault_text` field carrying the raw `$WR` string. That string is **already emitted** by `JuiceBoxBackend::addStatusFields` as the `wr` key (`src/lite/juicebox_backend.cpp:91`). To stay DRY we reuse the existing `wr` key as the fault detail rather than adding a duplicate `fault_text`. No new backend getter is introduced for it.

---

## Task 1: `LiteSessionEnergy` pure accumulator

**Files:**
- Create: `src/lite/lite_session_energy.h`
- Create: `src/lite/lite_session_energy.cpp`
- Test: `test/test_lite_session_energy/test_lite_session_energy.cpp`

- [ ] **Step 1: Write the failing test**

Create `test/test_lite_session_energy/test_lite_session_energy.cpp`:

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../src/lite/lite_session_energy.h"

TEST_CASE("accumulates watt-seconds and Wh over a charging interval") {
  LiteSessionEnergy e;
  e.tick(0, true, 0);              // rising edge into charging -> session starts at t=0
  e.tick(3680, true, 3600000);     // 3680 W held for 1 h (3,600,000 ms)
  CHECK(e.wattSeconds() == 13248000u);  // 3680 * 3600 s
  CHECK(e.wattHours()   == 3680u);      // 13,248,000 / 3600
}

TEST_CASE("elapsed tracks wall time from session start") {
  LiteSessionEnergy e;
  e.tick(1000, true, 1000);        // rising edge: session start = 1000 ms
  e.tick(1000, true, 31000);       // +30 s
  CHECK(e.elapsedSecs() == 30u);
}

TEST_CASE("rising edge into charging resets the session") {
  LiteSessionEnergy e;
  e.tick(0, true, 0);
  e.tick(3600, true, 3600000);     // accrue some energy
  CHECK(e.wattHours() == 3600u);
  e.tick(3600, false, 3700000);    // stop charging
  e.tick(0, true, 4000000);        // new rising edge -> fresh session
  e.tick(1000, true, 4000000 + 3600000);
  CHECK(e.wattHours() == 1000u);   // only the new session counts
  CHECK(e.elapsedSecs() == 3600u);
}

TEST_CASE("no accrual when idle or not charging") {
  LiteSessionEnergy e;
  e.tick(0, true, 0);
  e.tick(0, true, 3600000);        // charging but zero power
  CHECK(e.wattSeconds() == 0u);
  CHECK(e.elapsedSecs() == 3600u); // elapsed still advances while charging
  LiteSessionEnergy e2;
  e2.tick(3000, false, 0);
  e2.tick(3000, false, 3600000);   // never charging
  CHECK(e2.wattSeconds() == 0u);
  CHECK(e2.elapsedSecs() == 0u);
}

TEST_CASE("stop freezes the session total (does not zero)") {
  LiteSessionEnergy e;
  e.tick(0, true, 0);
  e.tick(3600, true, 3600000);
  e.tick(3600, false, 3700000);    // stop
  CHECK(e.wattHours() == 3600u);   // frozen, not reset
}

TEST_CASE("long high-power session does not overflow") {
  LiteSessionEnergy e;
  e.tick(0, true, 0);
  // 19.2 kW for 24 h = 460,800 Wh -> 1,658,880,000 Ws (exceeds uint32 if mishandled)
  e.tick(19200, true, 86400000u);
  CHECK(e.wattHours() == 460800u);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd /home/rar/oevse/openevse-juicebox-lite && pio test -e native -f test_lite_session_energy`
Expected: FAIL — `lite_session_energy.h` not found / `LiteSessionEnergy` undefined.

- [ ] **Step 3: Write the header**

Create `src/lite/lite_session_energy.h`:

```cpp
#pragma once
#include <stdint.h>

// Pure host-side session-energy accumulator. No Arduino / backend dependency —
// the caller supplies instantaneous power, a charging flag, and a monotonic
// millisecond clock. Integrates power over the interval between ticks.
//
// Session boundary: a rising edge into charging (charging goes false -> true)
// starts a fresh session, zeroing energy + elapsed. Stop (charging -> false)
// freezes the accumulated totals until the next rising edge.
class LiteSessionEnergy {
public:
  LiteSessionEnergy() { reset(); }

  // Advance the accumulator. now_ms is a millis()-style monotonic clock;
  // unsigned subtraction handles the ~49-day wraparound correctly.
  void tick(int power_w, bool charging, uint32_t now_ms);

  uint32_t wattSeconds() const { return (uint32_t)_wattSeconds; }       // session Ws
  uint32_t wattHours()   const { return (uint32_t)(_wattSeconds / 3600.0); } // session Wh
  uint32_t elapsedSecs() const { return _elapsedSecs; }                // s since session start

  void reset();

private:
  double   _wattSeconds;   // wide accumulator: avoids overflow on long sessions
  uint32_t _elapsedSecs;
  uint32_t _sessionStartMs;
  uint32_t _lastTickMs;
  bool     _prevCharging;
  bool     _haveTick;      // false until the first tick provides a baseline timestamp
};
```

- [ ] **Step 4: Write the implementation**

Create `src/lite/lite_session_energy.cpp`:

```cpp
#include "lite_session_energy.h"

void LiteSessionEnergy::reset() {
  _wattSeconds    = 0.0;
  _elapsedSecs    = 0;
  _sessionStartMs = 0;
  _lastTickMs     = 0;
  _prevCharging   = false;
  _haveTick       = false;
}

void LiteSessionEnergy::tick(int power_w, bool charging, uint32_t now_ms) {
  // Rising edge into charging -> start a fresh session.
  if (charging && !_prevCharging) {
    _wattSeconds    = 0.0;
    _elapsedSecs    = 0;
    _sessionStartMs = now_ms;
  }

  // Integrate only while charging and once we have a prior timestamp.
  if (charging && _haveTick) {
    uint32_t dt_ms = now_ms - _lastTickMs;   // unsigned: wraps correctly
    if (power_w > 0) {
      _wattSeconds += (double)power_w * (double)dt_ms / 1000.0;
    }
    _elapsedSecs = (now_ms - _sessionStartMs) / 1000;
  }

  _lastTickMs   = now_ms;
  _haveTick     = true;
  _prevCharging = charging;
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `cd /home/rar/oevse/openevse-juicebox-lite && pio test -e native -f test_lite_session_energy`
Expected: PASS — all 6 test cases.

- [ ] **Step 6: Commit**

```bash
cd /home/rar/oevse/openevse-juicebox-lite
git add src/lite/lite_session_energy.h src/lite/lite_session_energy.cpp test/test_lite_session_energy/
git commit -m "feat(lite): pure LiteSessionEnergy accumulator (Slice 3a)"
```

---

## Task 2: `lite_openevse_compat` state mapping

**Files:**
- Create: `src/lite/lite_openevse_compat.h`
- Create: `src/lite/lite_openevse_compat.cpp`
- Test: `test/test_lite_openevse_compat/test_lite_openevse_compat.cpp`

This unit depends only on the `LiteEvseState` enum (`src/lite/lite_evse_state.h`), which is pure (`#include <stdint.h>` only) and native-safe.

- [ ] **Step 1: Write the failing test**

Create `test/test_lite_openevse_compat/test_lite_openevse_compat.cpp`:

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include <string.h>
#include "../../src/lite/lite_openevse_compat.h"

TEST_CASE("maps canonical states to OpenEVSE state ints") {
  CHECK(openevse_state_code(LiteEvseState::Unknown,      false) == 0);
  CHECK(openevse_state_code(LiteEvseState::NotConnected, false) == 1);
  CHECK(openevse_state_code(LiteEvseState::Connected,    false) == 2);
  CHECK(openevse_state_code(LiteEvseState::Charging,     false) == 3);
  CHECK(openevse_state_code(LiteEvseState::Error,        false) == 8);
}

TEST_CASE("control-disabled overrides physical state with sleeping (254)") {
  CHECK(openevse_state_code(LiteEvseState::Charging,     true) == 254);
  CHECK(openevse_state_code(LiteEvseState::NotConnected, true) == 254);
  CHECK(openevse_state_code(LiteEvseState::Error,        true) == 8); // fault still wins
}

TEST_CASE("status strings match the state mapping") {
  CHECK(strcmp(openevse_status_str(LiteEvseState::Unknown,      false), "unknown") == 0);
  CHECK(strcmp(openevse_status_str(LiteEvseState::NotConnected, false), "not connected") == 0);
  CHECK(strcmp(openevse_status_str(LiteEvseState::Connected,    false), "connected") == 0);
  CHECK(strcmp(openevse_status_str(LiteEvseState::Charging,     false), "charging") == 0);
  CHECK(strcmp(openevse_status_str(LiteEvseState::Error,        false), "error") == 0);
  CHECK(strcmp(openevse_status_str(LiteEvseState::Charging,     true),  "sleeping") == 0);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd /home/rar/oevse/openevse-juicebox-lite && pio test -e native -f test_lite_openevse_compat`
Expected: FAIL — `lite_openevse_compat.h` not found.

- [ ] **Step 3: Write the header**

Create `src/lite/lite_openevse_compat.h`:

```cpp
#pragma once
#include "lite_evse_state.h"

// Map the canonical EVSE state onto the OpenEVSE local-API contract that the
// firstof9/openevse Home Assistant integration consumes (state int + status
// string). `controlDisabled` is the manager's control axis (manual Disabled
// claim): when set it reports OpenEVSE "sleeping" (254) UNLESS the device is
// faulted, in which case the fault wins.
//
// Error -> 8 is best-effort: the JuiceBox $-protocol only tells us fault != 0,
// not the OpenEVSE fault taxonomy. The raw $WR fault string is surfaced
// separately via the `wr` status field.
int         openevse_state_code(LiteEvseState s, bool controlDisabled);
const char *openevse_status_str(LiteEvseState s, bool controlDisabled);
```

- [ ] **Step 4: Write the implementation**

Create `src/lite/lite_openevse_compat.cpp`:

```cpp
#include "lite_openevse_compat.h"

int openevse_state_code(LiteEvseState s, bool controlDisabled) {
  if (s == LiteEvseState::Error) return 8;            // fault wins over control state
  if (controlDisabled)           return 254;          // sleeping
  switch (s) {
    case LiteEvseState::NotConnected: return 1;
    case LiteEvseState::Connected:    return 2;
    case LiteEvseState::Charging:     return 3;
    default:                          return 0;        // Unknown
  }
}

const char *openevse_status_str(LiteEvseState s, bool controlDisabled) {
  if (s == LiteEvseState::Error) return "error";
  if (controlDisabled)           return "sleeping";
  switch (s) {
    case LiteEvseState::NotConnected: return "not connected";
    case LiteEvseState::Connected:    return "connected";
    case LiteEvseState::Charging:     return "charging";
    default:                          return "unknown";
  }
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `cd /home/rar/oevse/openevse-juicebox-lite && pio test -e native -f test_lite_openevse_compat`
Expected: PASS — all 3 test cases.

- [ ] **Step 6: Commit**

```bash
cd /home/rar/oevse/openevse-juicebox-lite
git add src/lite/lite_openevse_compat.h src/lite/lite_openevse_compat.cpp test/test_lite_openevse_compat/
git commit -m "feat(lite): pure OpenEVSE state/status mapping (Slice 3a)"
```

---

## Task 3: Wire accumulator + new getters into `LiteEvseManager`

**Files:**
- Modify: `src/lite/lite_evse_manager.h`
- Modify: `src/lite/lite_evse_manager.cpp`

No native test: `LiteEvseManager` includes `MicroTasks.h` (device-only). Behavior is verified on-device in Task 6; the integration math (accumulator) is already covered by Task 1.

- [ ] **Step 1: Add includes, member, getters, and `loop()` to the header**

In `src/lite/lite_evse_manager.h`, add the accumulator include after the existing includes (after `#include "lite_charge_policy.h"`):

```cpp
#include "lite_session_energy.h"
```

Add this public method near the other `void` control methods (after `setTargetMaxCurrent(...)`):

```cpp
  // Periodic tick: ticks the session-energy accumulator from live backend power.
  // Call once per main-loop iteration (main_lite.cpp), alongside backend.loop().
  void loop();
```

Add these getters to the existing block of delegating getters (after `int getEvseState() const { ... }`):

```cpp
  int           getPower() const       { return _backend.getPower(); }
  LiteEvseState getDeviceState() const { return _backend.getState(); }

  uint32_t getSessionWattSeconds() const { return _energy.wattSeconds(); }
  uint32_t getSessionWattHours()   const { return _energy.wattHours(); }
  uint32_t getSessionElapsed()     const { return _energy.elapsedSecs(); }
```

Add the member to the private section (after `LiteEvseBackend &_backend;`):

```cpp
  LiteSessionEnergy _energy;
```

- [ ] **Step 2: Implement `loop()` in the cpp**

In `src/lite/lite_evse_manager.cpp`, add `#include <Arduino.h>` **inside** the `#ifdef OPENEVSE_LITE` guard (immediately after the existing `#include "lite_evse_manager.h"` line — NOT before the `#ifdef`, or the native build will try to parse Arduino.h), for `millis()`. Then add the method (place after `apply()`):

```cpp
void LiteEvseManager::loop() {
  _energy.tick(_backend.getPower(), _backend.isCharging(), millis());
}
```

- [ ] **Step 3: Build the device firmware to verify it compiles**

Run: `cd /home/rar/oevse/openevse-juicebox-lite && pio run -e openevse_lite 2>&1 | tail -20`
Expected: build succeeds (`SUCCESS`). The manager compiles with the new member/getters/loop.

- [ ] **Step 4: Commit**

```bash
cd /home/rar/oevse/openevse-juicebox-lite
git add src/lite/lite_evse_manager.h src/lite/lite_evse_manager.cpp
git commit -m "feat(lite): LiteEvseManager owns session energy + power/state getters (Slice 3a)"
```

---

## Task 4: Tick `manager.loop()` from `main_lite.cpp`

**Files:**
- Modify: `src/lite/main_lite.cpp:loop()`

- [ ] **Step 1: Add the manager tick**

In `src/lite/main_lite.cpp`, change the `loop()` body from:

```cpp
void loop()
{
  web_server_lite_loop();
  s_backend.loop();
}
```

to:

```cpp
void loop()
{
  web_server_lite_loop();
  s_backend.loop();
  s_manager.loop();   // tick session-energy accumulator from live backend power
}
```

- [ ] **Step 2: Build to verify it compiles**

Run: `cd /home/rar/oevse/openevse-juicebox-lite && pio run -e openevse_lite 2>&1 | tail -20`
Expected: build succeeds.

- [ ] **Step 3: Commit**

```bash
cd /home/rar/oevse/openevse-juicebox-lite
git add src/lite/main_lite.cpp
git commit -m "feat(lite): tick manager.loop() for session energy (Slice 3a)"
```

---

## Task 5: Expand `build_status_json` to the OpenEVSE key set

**Files:**
- Modify: `src/lite/web_server_lite.cpp`

- [ ] **Step 1: Add includes**

In `src/lite/web_server_lite.cpp`, in the pre-mongoose include block (the C++ headers before `#include "mongoose.h"`), add `WiFi.h` and the compat header. Add after `#include "lite_charge_policy.h"`:

```cpp
#include <WiFi.h>
#include "lite_openevse_compat.h"
```

(Include order matters: these must precede `#include "mongoose.h"` — see the existing comment at the top of the file.)

- [ ] **Step 2: Add a firmware-version macro**

Near the top of the file, after the includes, add:

```cpp
// Reported as OpenEVSE `firmware`/`version` so the HA integration shows a value.
#ifndef LITE_FW_VERSION
#define LITE_FW_VERSION "lite-3a"
#endif
```

- [ ] **Step 3: Replace `build_status_json`**

Replace the entire existing `build_status_json` function (currently `src/lite/web_server_lite.cpp:37-54`) with:

```cpp
// Build the /status JSON in the OpenEVSE local-API shape the firstof9/openevse
// Home Assistant integration consumes (it polls this every 60 s). Keys it reads
// but a JuiceBox can't provide (divert/shaper/OCPP/GFCI counts/etc.) are simply
// omitted — the integration's .get() defaults tolerate absent keys.
static void build_status_json(String &out)
{
  StaticJsonDocument<1024> doc;

  if (s_mgr_ctrl) {
    LiteEvseState dev   = s_mgr_ctrl->getDeviceState();
    bool disabled       = (s_mgr_ctrl->getState() == EvseState::Disabled);

    // State (int + string), per the OpenEVSE contract.
    doc["state"]  = openevse_state_code(dev, disabled);
    doc["status"] = openevse_status_str(dev, disabled);

    // Live telemetry.
    int amp   = s_mgr_ctrl->getAmps();
    int power = s_mgr_ctrl->getPower();
    doc["amp"]               = amp;
    doc["pilot"]             = (uint32_t)s_mgr_ctrl->getChargeCurrent(); // advertised setpoint
    doc["power"]             = power;
    doc["tempt"]             = s_mgr_ctrl->getTemperature();
    doc["temp2"]             = s_mgr_ctrl->getTemperature();
    doc["max_current_soft"]  = (uint32_t)s_mgr_ctrl->getChargeCurrent();
    doc["max_current_hard"]  = s_mgr_ctrl->getMaxHardwareCurrent();
    doc["min_current_hard"]  = s_mgr_ctrl->getMinCurrent();
    doc["available_current"] = (uint32_t)s_mgr_ctrl->getMaxCurrent();
    doc["manual_override"]   = manual.isActive() ? 1 : 0;
    doc["mode"]              = "fast";

    // Derived voltage: power / amps while charging, else nominal 240 V. Keeps
    // HA's V x I ~= power consistent without a sensor the JuiceBox lacks.
    doc["voltage"] = (amp > 0 && power > 0) ? (power / amp) : 240;

    // Session energy (host-side accumulator).
    doc["wattsec"]        = (uint32_t)s_mgr_ctrl->getSessionWattSeconds();
    doc["watthour"]       = (uint32_t)s_mgr_ctrl->getSessionWattHours();
    doc["session_energy"] = (uint32_t)s_mgr_ctrl->getSessionWattHours();
    doc["elapsed"]        = (uint32_t)s_mgr_ctrl->getSessionElapsed();

    // Backend-specific extras (hw/fw/protocol/md/wc/wr/line + state_str). The
    // `wr` key carries the raw $WR fault string (the fault detail for state 8).
    s_mgr_ctrl->addStatusFields(doc);

    // Control/claim diagnostics (retained from the prior status body).
    doc["claims"] = (uint32_t)s_mgr_ctrl->activeClaimCount();
    doc["manual"] = manual.isActive() ? 1 : 0;
  }

  // Identity / system.
  doc["firmware"]  = LITE_FW_VERSION;
  doc["version"]   = LITE_FW_VERSION;
  doc["ipaddress"] = WiFi.localIP().toString();
  doc["ssid"]      = WiFi.SSID();
  doc["srssi"]     = WiFi.RSSI();
  doc["free_heap"] = ESPAL.getFreeHeap();
  doc["freeram"]   = ESPAL.getFreeHeap();
  doc["uptime"]    = (uint32_t)(millis() / 1000);

  serializeJson(doc, out);
}
```

- [ ] **Step 4: Build to verify it compiles**

Run: `cd /home/rar/oevse/openevse-juicebox-lite && pio run -e openevse_lite 2>&1 | tail -20`
Expected: build succeeds. Watch for ArduinoJson document-overflow at runtime (Task 6 checks the served body is complete); 1024 bytes covers ~30 fields + the addStatusFields strings.

- [ ] **Step 5: Commit**

```bash
cd /home/rar/oevse/openevse-juicebox-lite
git add src/lite/web_server_lite.cpp
git commit -m "feat(lite): OpenEVSE-compatible GET /status body (Slice 3a)"
```

---

## Task 6: Full build + on-device validation

**Files:** none (validation only)

- [ ] **Step 1: Run the full native test suite**

Run: `cd /home/rar/oevse/openevse-juicebox-lite && pio test -e native 2>&1 | tail -30`
Expected: all suites pass, including `test_lite_session_energy` and `test_lite_openevse_compat`.

- [ ] **Step 2: Build the device firmware**

Run: `cd /home/rar/oevse/openevse-juicebox-lite && pio run -e openevse_lite 2>&1 | tail -25`
Expected: `SUCCESS`. Note the flash/RAM figures from the output for the slice record.

- [ ] **Step 3: Flash and hardware-validate (user-driven)**

This step requires the bench (J-Link + powered JuiceBox). Hand off to the user with these checks:

1. Flash via `scripts/lite_flash.sh` (build creates `firmware.bin`; flash bootloader.bin@0x0 + firmware.bin@0x8000 — see the flash-procedure memory). Supply WiFi creds via `PLATFORMIO_BUILD_FLAGS` single-quote-wrapped.
2. `curl http://<board-ip>/status` — confirm the full key set is present and values are live (`state`, `status`, `amp`, `power`, `voltage`, `tempt`, `max_current_*`, `wattsec`/`watthour`/`session_energy`/`elapsed`, identity fields). Confirm the JSON is complete (not truncated — guards against doc-size overflow).
3. With the vehicle charging: confirm `session_energy`/`elapsed` climb over time and `state` reads `3`/`"charging"`.
4. Add the firstof9/openevse integration in Home Assistant pointed at the board IP. Confirm it sets up (60 s poll, no `ConfigEntryNotReady`) and surfaces state, current, power, session energy, and temperature entities.

- [ ] **Step 4: Record outcome**

Update the slice record (memory `juicebox-lite-reuse-roadmap.md`) with the build figures and HW-validation result. Do **not** push unless the user asks.

---

## Self-Review

**1. Spec coverage:**
- Pure `LiteSessionEnergy` accumulator (Wh + elapsed, reset-on-edge) → Task 1. ✓
- State/status mapping incl. Disabled→254, Error→8 → Task 2. ✓
- Manager owns accumulator + `loop()` + session getters; `getPower`/`getDeviceState` gap → Task 3. ✓
- `main_lite.cpp` ticks `manager.loop()` → Task 4. ✓
- Full `/status` field contract (state, live telemetry, energy, derived voltage, identity; OpenEVSE-only keys omitted) → Task 5. ✓
- Native tests (accumulation, elapsed, reset, idle, freeze, overflow) → Task 1; mapping tests → Task 2. ✓
- On-device + HA integration validation → Task 6. ✓
- Deferred (lifetime totals, /ws, /override) correctly absent. ✓
- `fault_text` reconciled to existing `wr` key (documented in File Structure). ✓

**2. Placeholder scan:** No TBD/TODO/"handle edge cases". Every code step shows complete code. ✓

**3. Type consistency:** `LiteSessionEnergy` methods (`tick`, `wattSeconds`, `wattHours`, `elapsedSecs`, `reset`) consistent across Task 1 header/impl/test and Task 3 getter bodies. `openevse_state_code`/`openevse_status_str` signatures consistent across Task 2 and Task 5. Manager getters (`getPower`, `getDeviceState`, `getSession*`) defined in Task 3, consumed in Task 5. `EvseState::Disabled` and `getState()` match `lite_evse_manager.h`. `getMaxCurrent()`/`getChargeCurrent()` return `uint32_t` — cast at use sites. ✓
