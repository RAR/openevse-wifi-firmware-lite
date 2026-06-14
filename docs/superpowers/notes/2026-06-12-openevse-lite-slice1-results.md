# OpenEVSE Lite — Slice 1 Results (JuiceBox 40 / WGM160P / LibreTiny)

**Date:** 2026-06-12
**Branch:** `feature/juicebox-lite`
**Plan:** `docs/superpowers/plans/2026-06-12-openevse-lite-slice1.md`

## Outcome

Slice-1 goal reached at the build level: the **real OpenEVSE EVSE core** compiles and
links into the `openevse_lite` env on the Silicon Labs WGM160P (EFM32GG11 Cortex-M4F +
WF200) via the author's LibreTiny fork, driven by a minimal boot path that joins WiFi
(STA) and serves a **live `/status`** over the vendored Mongoose C core. **No FakeEVSE.**

Hardware acceptance (flash + `curl /status`) is author-run and pending — there is no
serial console this slice (USART0 is the RAPI line).

## Route 1 (Mongoose over LibreTiny lwIP sockets): **confirmed** (T1, HW-validated earlier)

## Flash / RAM budget (`pio run -e openevse_lite`)

```
RAM:   31.7% (166116 / 524288 bytes)
Flash: 24.4% (511032 / 2097152 bytes)
```

Comfortable headroom on the 2 MB part after the WF200 blob + (future) config region.
Link is clean (`LINK_OK`: no `undefined reference` / `error:`).

## Final `build_src_filter`

```
-<*>
+<lite/>
-<lite/spike_main.cpp>
-<lite/lite_config_store.cpp>
+<evse_man.cpp> +<evse_monitor.cpp> +<event_log.cpp> +<openevse.cpp>
+<energy_meter.cpp> +<input_filter.cpp>
```

## Final `lib_deps`

```
bblanchon/ArduinoJson@6.20.1
jeremypoulter/OpenEVSE@0.0.15
jeremypoulter/MicroTasks@0.0.4
jeremypoulter/Micro Debug@0.0.5
jeremypoulter/StreamSpy@0.0.2
```

(ConfigJson intentionally NOT included — `app_config.h` needs no ConfigJson header, and
pulling it drags `ConfigJson.cpp`, which needs an `EEPROM.h` the fork lacks.)

## Key design decisions (author-directed)

- **Keep the real EVSE core, drop FakeEVSE.** The JuiceBox has a real OpenEVSE
  controller; `EvseManager` talks to it over RAPI. FakeEVSE was a bench simulator, not
  needed to run.
- **RAPI = `SerialEvse` (StreamSpy) on `Serial` @ 9600 8N1 = USART0 LOC1 (PE7=TX/PE6=RX).**
  That is the only USART the LibreTiny EFM32 fork wires, and it is the controller line.
  Therefore **no separate debug console** this slice — any `Serial.print*` would corrupt
  RAPI framing. Observability = WiFi + `curl /status`. LibreTiny's own `LT_*` logging is
  silenced (`-D LT_LOGLEVEL=LT_LEVEL_NONE`).
- **Persistence deferred.** The fork ships no LittleFS (it has `Preferences`). A no-op
  `src/lite/LittleFS.h` shim satisfies `event_log`/`energy_meter` so they compile; nothing
  is persisted (the core tolerates "missing file = fresh/defaults"). WiFi creds come from
  the `LITE_WIFI_SSID/PASS` build flags. Slice-2 should back these with `Preferences`.

## Lite link shims (no fork / library sources modified)

| Shim | Why |
|------|-----|
| `src/lite/app_config_lite.cpp` | Defines the single config global `flags = CONFIG_DEFAULT_STATE` (→ `config_default_state()==Active`, `config_threephase_enabled()==false`). Avoids compiling the whole `app_config.cpp`/ConfigJson surface. |
| `src/lite/lite_evse_stubs.cpp` | No-op `event_send` + inert `divert`/`shaper` globals (excluded subsystems the EVSE core names). `isActive()/getState()==false`. |
| `src/lite/LittleFS.h` + `littlefs_lite.cpp` | No-op FS so `event_log`/`energy_meter` compile (persistence deferred). |
| `src/lite/debug_lite.cpp` | `SerialEvse`/`SerialDebug` StreamSpy wrappers on `Serial` @ 9600. |
| `scripts/lite_exclude_sources.py` | Build middleware skipping MicroTasks' un-portable, link-unneeded `MicroTasksInterrupt.cpp`. |
| build flags `-D interrupts=__enable_irq` / `-D noInterrupts=__disable_irq` | The EFM32 family `ArduinoFamily.h` omits these (other LibreTiny families define them); mapped to CMSIS PRIMASK ops. |

## LibreTiny fork follow-ups (fold back into PR #387 eventually)

- EFM32 `ArduinoFamily.h` omits `interrupts`/`noInterrupts` macros that the other families
  define — worked around with build flags here; cleaner to add upstream.
- No LittleFS / generic flash-FS for the EFM32 port — slice-2 persistence needs a real
  backing store (Preferences or a small flash region).

## Known minor (non-blocking)

- The Divert/Shaper stub member-init lists in `lite_evse_stubs.cpp` omit a couple of
  trailing members (`_evse_last_state`, `_inputFilter`, the Divert base `Task()`). Safe
  today (default-constructed / never read by the stubbed methods), but brittle against
  header drift / `-Wreorder -Werror`. Complete them if those members ever become live.

## Next slice (not slice-1)

- Wire the controller to PE7/PE6 and validate live `state`/`amp`/`voltage` over `curl /status`.
- `Preferences`-backed WiFi-cred + config store (re-target the deferred `lite_config_store`).
- Second USART for a debug console (frees USART0 to be RAPI-only with logging back on).
