# Slice 2: RGB LED EVSE-State Indicator — Design

**Date:** 2026-06-13
**Branch:** `feature/juicebox-lite`
**Worktree:** `/home/rar/oevse/openevse-juicebox-lite`

## Context

The JuiceBox 40 has an onboard active-high RGB LED (PB5 red / PB6 green / PD8 blue,
bench-verified). The WGM160P LibreTiny WiFi backend currently drives it as a bring-up
indicator (`ltWifiStatusLed`, the red→magenta→blue→green ladder). Now that the EVSE control
core is up, the LED should instead show **EVSE state** — the at-a-glance "is it charging /
plugged / faulted" indicator every charger has.

The WiFi backend exposes the exact handoff we need (user-directed): call
`ltWifiStatusLedEnable(false)` (declared in the WiFi library's `WiFiStatusLed.h`) to release
the RGB pins, after which `ltWifiStatusLed()` no-ops and the app owns `LED_R/LED_G/LED_B`.

## Goal

After WiFi is up, take over the RGB LED and drive it from the live EVSE state — a clear,
conventional color per state, with a blink for the two states that warrant attention
(charging activity, fault).

## Design decisions (documented per the autonomy mandate)

**D1 — pure color/pattern mapping, thin device driver.** The state→color+blink mapping and
the blink on/off phase math are pure and native-tested in a `lite_led` unit. The device glue
(disable WiFi LED, `pinMode`, per-loop `digitalWrite`) is the only non-testable part and is
minimal.

**D2 — color scheme (active-high RGB, intuitive EV semantics):**

| Condition (priority order)        | Color   | R G B | Pattern        |
|-----------------------------------|---------|-------|----------------|
| Backend offline (no Atmel comms)  | white   | 1 1 1 | slow blink     |
| Error / fault (S05 / GFI)         | red     | 1 0 0 | fast blink     |
| Policy-disabled (override/sched)  | yellow  | 1 1 0 | solid          |
| Charging                          | green   | 0 1 0 | slow blink (activity) |
| Connected (plugged, not charging) | cyan    | 0 1 1 | solid          |
| Not connected (idle/ready)        | blue    | 0 0 1 | solid          |
| Unknown                           | white   | 1 1 1 | solid          |

Priority is top-down: offline and error outrank everything (safety/attention); policy-
disabled outranks the device sub-states (so a manual/schedule stop reads yellow even with a
car plugged in). No PWM/breathing — `digitalWrite` on/off only (the EFM32 app-side blink is
a millis-gated toggle); keeps it portable and trivially testable.

**D3 — blink via millis-gated phase, computed in the pure unit.** `lite_led_phase_on(pattern,
nowMs)` returns whether the LED is in its "on" half right now: Solid→always on; SlowBlink→
~1 Hz (500 ms on/off); FastBlink→~3 Hz (~160 ms). The device loop calls it each iteration
and ANDs it with each channel.

**D4 — always-on for this board; guarded for others.** The device glue is wrapped in
`#if defined(LED_R) && defined(LED_G) && defined(LED_B)` so a board variant without the
macros compiles to nothing (mirrors `WiFiStatusLed`'s own no-op-on-absent-macros pattern).
No config flag — the indicator is a core UX feature, always on.

## Architecture

### New pure unit: `src/lite/lite_led.{h,cpp}`

```cpp
#include <stdint.h>
#include "lite_evse_state.h"   // LiteEvseState (pure header, no Arduino dep)

struct LiteLedColor { bool r, g, b; };
enum class LiteLedPattern : uint8_t { Solid, SlowBlink, FastBlink };
struct LiteLedSpec { LiteLedColor color; LiteLedPattern pattern; };

// Resolve the indicator for the current condition. Priority: offline > error >
// disabled > device sub-state. `disabled` is the policy state (manager target Disabled);
// `online` is backend comms liveness. Pure.
LiteLedSpec lite_led_for(LiteEvseState dev, bool disabled, bool online);

// Is the LED in its lit half at nowMs? Solid -> always true. Pure (no millis() call;
// the caller passes the clock so it's testable).
bool lite_led_phase_on(LiteLedPattern pattern, uint32_t nowMs);
```

`lite_led_for` logic (priority order from D2). `lite_led_phase_on`: `Solid`→true;
`SlowBlink`→`(nowMs / 500) & 1` (≈1 Hz); `FastBlink`→`(nowMs / 160) & 1` (≈3 Hz).

No `OPENEVSE_LITE` guard (compiles native). `.cpp` added to `[env:native]` build filter.
`lite_evse_state.h` is already pure (used in native tests via the backend), so the include
is native-safe.

### Modified: `src/lite/main_lite.cpp`

- `#include "lite_led.h"`. After WiFi connect + `web_server_lite_begin`, in `setup()`:
  ```cpp
  #if defined(LED_R) && defined(LED_G) && defined(LED_B)
    ltWifiStatusLedEnable(false);      // release the RGB LED from the WiFi backend
    pinMode(LED_R, OUTPUT); pinMode(LED_G, OUTPUT); pinMode(LED_B, OUTPUT);
  #endif
  ```
  (`ltWifiStatusLedEnable` is declared in `WiFiStatusLed.h` — include it; it sits in the WiFi
  library already on the include path via `<WiFi.h>`.)
- In `loop()`, after `s_manager.loop()`, drive the LED:
  ```cpp
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
  (`s_manager.getState()` with no client returns the arbitrated target state; `getDeviceState()`
  is the backend's live `LiteEvseState`; `s_backend.isOnline()` is comms liveness.)

### `platformio.ini`

Add `+<lite/lite_led.cpp>` to `[env:native]` build_src_filter.

## Data flow

```
each loop: (getDeviceState, target==Disabled, isOnline)
   -> lite_led_for -> {color, pattern}
   -> lite_led_phase_on(pattern, millis()) -> on?
   -> digitalWrite LED_R/G/B = color.* && on   (active-high)
```

## Error handling / edge cases

- **Board without LED_R/G/B** → entire glue compiled out (D4); pure unit still builds/tests.
- **WiFi LED race** → `ltWifiStatusLedEnable(false)` is called from the app thread before the
  app first drives the pins; WiFi LED writes only happen on connection-state changes, so the
  disable reliably wins (per the WiFiStatusLed.h contract). We call it once, post-connect.
- **Offline at boot** (Atmel silent) → `isOnline()` false → white slow-blink, which is a
  useful "no controller" signal (distinct from the WiFi backend's old red/magenta).

## Testing

### Native (doctest) — `test/test_lite_led/`

- `lite_led_for`: each row of the D2 table → expected color + pattern. Priority checks:
  offline+error → white-blink (offline wins); error+charging → red; disabled+connected →
  yellow; disabled+charging → yellow (policy outranks charging); charging (online, not
  disabled) → green slow-blink; connected → cyan; not_connected → blue; unknown → white solid.
- `lite_led_phase_on`: Solid → true at any t; SlowBlink → on in [0,500), off in [500,1000),
  on again at 1000; FastBlink → toggles at the 160 ms boundary; both correct across a large t
  (no overflow surprise).

### On-device (bench-checkable NOW — LED is independent of the faulted Atmel)

Flash; confirm the LED takes over from the WiFi ladder after join (no longer stuck on the
WiFi green), and shows: **white slow-blink** while the bench Atmel is silent/offline, or
**red blink** if it reports the GFI fault (S05). The charging/connected colors need a live
vehicle (deferred to a complete unit), but offline/error/idle are directly observable. This
is the first lite slice with bench-visible output independent of the GFI fault.

## Files

- **Create:** `src/lite/lite_led.{h,cpp}`, `test/test_lite_led/`
- **Modify:** `src/lite/main_lite.cpp` (disable WiFi LED + pinMode in setup; drive in loop)
- **Modify:** `platformio.ini` (`+<lite/lite_led.cpp>` on `[env:native]`)
