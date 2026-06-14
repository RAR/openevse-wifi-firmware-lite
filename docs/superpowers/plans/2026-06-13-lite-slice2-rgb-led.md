# Slice 2: RGB LED EVSE-State Indicator Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** After WiFi is up, take over the JuiceBox 40's onboard RGB LED and drive it from live EVSE state — conventional color per state, blink for charging-activity and fault.

**Architecture:** A pure native-tested `lite_led` unit (state→color+pattern map + millis-gated blink phase); thin device glue in `main_lite.cpp` that releases the WiFi backend's LED (`ltWifiStatusLedEnable(false)`) and `digitalWrite`s `LED_R/LED_G/LED_B` each loop.

**Tech Stack:** C++17, LibreTiny/EFM32GG11 Arduino (`pinMode`/`digitalWrite`/`LED_R/G/B`, `WiFiStatusLed.h`), doctest (native), PlatformIO.

**Reference:** `docs/superpowers/specs/2026-06-13-lite-slice2-rgb-led-design.md`

---

## File Structure

- **Create** `src/lite/lite_led.h` / `.cpp` — pure indicator mapping.
- **Create** `test/test_lite_led/test_lite_led.cpp` — doctest suite.
- **Modify** `src/lite/main_lite.cpp` — disable WiFi LED + pinMode (setup); drive LED (loop).
- **Modify** `platformio.ini` — `+<lite/lite_led.cpp>` on `[env:native]`.

---

### Task 1: Pure `lite_led` unit (TDD, native)

**Files:**
- Create: `src/lite/lite_led.h`, `src/lite/lite_led.cpp`
- Test: `test/test_lite_led/test_lite_led.cpp`
- Modify: `platformio.ini` (`[env:native]` build_src_filter)

- [ ] **Step 1: Header** — create `src/lite/lite_led.h`:

```cpp
#pragma once
#include <stdint.h>
#include "lite_evse_state.h"

struct LiteLedColor { bool r, g, b; };
enum class LiteLedPattern : uint8_t { Solid, SlowBlink, FastBlink };
struct LiteLedSpec { LiteLedColor color; LiteLedPattern pattern; };

// Resolve the indicator for the current condition. Priority: offline > error > disabled >
// device sub-state. `disabled` = policy state (manager target Disabled); `online` = backend
// comms liveness. Pure.
LiteLedSpec lite_led_for(LiteEvseState dev, bool disabled, bool online);

// Is the LED in its lit half at nowMs? Solid -> always true. Pure.
bool lite_led_phase_on(LiteLedPattern pattern, uint32_t nowMs);
```

- [ ] **Step 2: Failing test** — create `test/test_lite_led/test_lite_led.cpp`:

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../src/lite/lite_led.h"

static bool colEq(LiteLedColor c, bool r, bool g, bool b) {
  return c.r == r && c.g == g && c.b == b;
}

TEST_CASE("offline outranks everything -> white slow-blink") {
  LiteLedSpec s = lite_led_for(LiteEvseState::Error, true, false); // error+disabled+offline
  CHECK(colEq(s.color, 1, 1, 1));
  CHECK(s.pattern == LiteLedPattern::SlowBlink);
}

TEST_CASE("error (online) -> red fast-blink, outranks charging/disabled") {
  LiteLedSpec s = lite_led_for(LiteEvseState::Error, true, true);
  CHECK(colEq(s.color, 1, 0, 0));
  CHECK(s.pattern == LiteLedPattern::FastBlink);
}

TEST_CASE("policy-disabled outranks device sub-state -> yellow solid") {
  CHECK(colEq(lite_led_for(LiteEvseState::Connected, true, true).color, 1, 1, 0));
  CHECK(colEq(lite_led_for(LiteEvseState::Charging,  true, true).color, 1, 1, 0)); // even charging
  CHECK(lite_led_for(LiteEvseState::Connected, true, true).pattern == LiteLedPattern::Solid);
}

TEST_CASE("charging -> green slow-blink") {
  LiteLedSpec s = lite_led_for(LiteEvseState::Charging, false, true);
  CHECK(colEq(s.color, 0, 1, 0));
  CHECK(s.pattern == LiteLedPattern::SlowBlink);
}

TEST_CASE("connected -> cyan solid; not_connected -> blue solid") {
  CHECK(colEq(lite_led_for(LiteEvseState::Connected,    false, true).color, 0, 1, 1));
  CHECK(colEq(lite_led_for(LiteEvseState::NotConnected, false, true).color, 0, 0, 1));
  CHECK(lite_led_for(LiteEvseState::NotConnected, false, true).pattern == LiteLedPattern::Solid);
}

TEST_CASE("unknown (online) -> white solid") {
  LiteLedSpec s = lite_led_for(LiteEvseState::Unknown, false, true);
  CHECK(colEq(s.color, 1, 1, 1));
  CHECK(s.pattern == LiteLedPattern::Solid);
}

TEST_CASE("phase_on: solid always on") {
  CHECK(lite_led_phase_on(LiteLedPattern::Solid, 0));
  CHECK(lite_led_phase_on(LiteLedPattern::Solid, 1234567));
}

TEST_CASE("phase_on: slow blink ~1Hz (500ms half)") {
  CHECK(lite_led_phase_on(LiteLedPattern::SlowBlink, 0));        // on
  CHECK(lite_led_phase_on(LiteLedPattern::SlowBlink, 499));      // on
  CHECK_FALSE(lite_led_phase_on(LiteLedPattern::SlowBlink, 500)); // off
  CHECK_FALSE(lite_led_phase_on(LiteLedPattern::SlowBlink, 999)); // off
  CHECK(lite_led_phase_on(LiteLedPattern::SlowBlink, 1000));     // on again
}

TEST_CASE("phase_on: fast blink toggles at 160ms") {
  CHECK(lite_led_phase_on(LiteLedPattern::FastBlink, 0));
  CHECK_FALSE(lite_led_phase_on(LiteLedPattern::FastBlink, 160));
  CHECK(lite_led_phase_on(LiteLedPattern::FastBlink, 320));
}
```

- [ ] **Step 3: Run, verify fail** — `pio test -e native -f test_lite_led` → FAIL (undefined refs).

- [ ] **Step 4: Implement** — create `src/lite/lite_led.cpp`:

```cpp
#include "lite_led.h"

LiteLedSpec lite_led_for(LiteEvseState dev, bool disabled, bool online) {
  // Priority order: offline > error > policy-disabled > device sub-state.
  if (!online)                    return { {1, 1, 1}, LiteLedPattern::SlowBlink }; // white blink
  if (dev == LiteEvseState::Error) return { {1, 0, 0}, LiteLedPattern::FastBlink }; // red blink
  if (disabled)                   return { {1, 1, 0}, LiteLedPattern::Solid };     // yellow
  switch (dev) {
    case LiteEvseState::Charging:     return { {0, 1, 0}, LiteLedPattern::SlowBlink }; // green
    case LiteEvseState::Connected:    return { {0, 1, 1}, LiteLedPattern::Solid };     // cyan
    case LiteEvseState::NotConnected: return { {0, 0, 1}, LiteLedPattern::Solid };     // blue
    default:                          return { {1, 1, 1}, LiteLedPattern::Solid };     // white
  }
}

bool lite_led_phase_on(LiteLedPattern pattern, uint32_t nowMs) {
  switch (pattern) {
    case LiteLedPattern::SlowBlink: return ((nowMs / 500u) & 1u) == 0u; // 500ms on/off
    case LiteLedPattern::FastBlink: return ((nowMs / 160u) & 1u) == 0u; // ~160ms on/off
    case LiteLedPattern::Solid:
    default:                        return true;
  }
}
```

- [ ] **Step 5: Native build filter** — append `+<lite/lite_led.cpp>` to the `[env:native]` `build_src_filter` line in `platformio.ini`.

- [ ] **Step 6: Run, verify pass** — `pio test -e native -f test_lite_led` → PASS.

- [ ] **Step 7: Commit**

```bash
git add src/lite/lite_led.h src/lite/lite_led.cpp test/test_lite_led/ platformio.ini
git commit -m "feat(lite): pure RGB-LED state->color+blink mapping (native-tested)"
```

---

### Task 2: Drive the LED from main_lite

**Files:**
- Modify: `src/lite/main_lite.cpp`

- [ ] **Step 1: Includes** — add near the other lite includes (after `#include "lite_led.h"` does not exist yet, so add it):

```cpp
#include "lite_led.h"
#include "WiFiStatusLed.h"   // ltWifiStatusLedEnable — WiFi lib header, on the path via <WiFi.h>
```

If `#include "WiFiStatusLed.h"` does not resolve, use the angle form `#include <WiFiStatusLed.h>`; if still unresolved, declare the prototype locally: `extern "C" void ltWifiStatusLedEnable(bool);` is NOT correct (it's C++ linkage) — instead add `void ltWifiStatusLedEnable(bool);` as a forward declaration. Prefer the header include; only fall back if the build can't find it.

- [ ] **Step 2: Take over the LED in `setup()`** — at the END of `setup()` (after the PF11 RESET release sequence, after the final `delay(100);` and before the closing `}`), add:

```cpp
  // Take ownership of the RGB LED from the WiFi backend and show EVSE state instead of
  // the WiFi bring-up ladder. ltWifiStatusLedEnable(false) makes the WiFi LED calls no-op.
#if defined(LED_R) && defined(LED_G) && defined(LED_B)
  ltWifiStatusLedEnable(false);
  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);
#endif
```

- [ ] **Step 3: Drive the LED in `loop()`** — at the END of `loop()` (after `s_wasCharging = charging;`, before the closing `}`), add:

```cpp
  // EVSE-state RGB indicator (active-high). Pure mapping in lite_led; this is the only
  // device-side glue. Compiled out on boards without the LED_R/G/B variant macros.
#if defined(LED_R) && defined(LED_G) && defined(LED_B)
  {
    LiteLedSpec spec = lite_led_for(s_manager.getDeviceState(),
                                    s_manager.getState() == EvseState::Disabled,
                                    s_backend.isOnline());
    bool on = lite_led_phase_on(spec.pattern, millis());
    digitalWrite(LED_R, (spec.color.r && on) ? HIGH : LOW);
    digitalWrite(LED_G, (spec.color.g && on) ? HIGH : LOW);
    digitalWrite(LED_B, (spec.color.b && on) ? HIGH : LOW);
  }
#endif
```

NOTE: `s_manager.getState()` (no-arg) returns the arbitrated target `EvseState`;
`EvseState` is already in scope in main_lite via `manual.h`. `getDeviceState()` returns the
backend `LiteEvseState`. `s_backend.isOnline()` is the JuiceBox comms-liveness flag.

- [ ] **Step 4: Build device env** — `pio run -e openevse_lite` → SUCCESS. Record flash %.
  If `ltWifiStatusLedEnable` is an unresolved symbol at link, the WiFi library providing it
  isn't linked into this env — STOP and report (it should be: the env uses `<WiFi.h>`).

- [ ] **Step 5: Commit**

```bash
git add src/lite/main_lite.cpp
git commit -m "feat(lite): drive onboard RGB LED from EVSE state (take over WiFi status LED)"
```

---

### Task 3: Full native suite + production build verification

**Files:** none (verification only)

- [ ] **Step 1: Full native suite** — `pio test -e native` → ALL PASS (incl. `test_lite_led`).

- [ ] **Step 2: Production build** — `pio run -e openevse_lite` → SUCCESS; record flash % (was 24.1% after Slice 4).

- [ ] **Step 3: Confirm the WiFi LED handoff** — grep `main_lite.cpp` for `ltWifiStatusLedEnable(false)` (present, in setup) and the loop `digitalWrite(LED_R` block (present). The WiFi backend's `ltWifiStatusLed()` calls are now no-ops post-handoff.

- [ ] **Step 4:** Slice 2 is code-complete + native-tested. On-device: the offline (white slow-blink) and error (red fast-blink) states are bench-observable now (independent of the GFI-faulted Atmel — the LED is host-driven); charging/connected colors need a live vehicle (deferred to a complete unit).
