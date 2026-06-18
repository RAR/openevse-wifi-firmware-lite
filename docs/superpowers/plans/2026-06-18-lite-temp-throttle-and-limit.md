# Lite Temp-Throttle & Session-Limit — Implementation Plan

> Executes `docs/superpowers/specs/2026-06-18-lite-temp-throttle-and-limit-design.md`. Two features, built sequentially (shared files: `web_server_lite.cpp`, `lite_config_store.*`, `platformio.ini`). TDD on the pure units; device-build-verified; commit per feature.

**Goal:** Temp-throttle + `/limit` session-limit parity on the lite control seam.
**Tech:** C++17, doctest (native), ArduinoJson 6, LiteEvseManager claim seam.

---

## Task 1: Temp-throttle pure unit + native test

**Files:** Create `src/lite/lite_temp_throttle.{h,cpp}`, `test/test_lite_temp_throttle/test_main.cpp`; Modify `platformio.ini` (native filter).

- [ ] Write failing doctest: engage at setpoint while charging; decrement toward floor; no-engage when idle/pilot0; recover +1/loop; release when recovered; disabled→reset+Release; invalid-temp→None.
- [ ] Add `+<lite/lite_temp_throttle.cpp>` to `[env:native]` build_src_filter; run `pio test -e native` → FAIL (undefined).
- [ ] Implement `lite_temp_throttle_tick` per spec (transcribe `temp_throttle.cpp`, double→int).
- [ ] `pio test -e native` → PASS.

## Task 2: Temp-throttle config store

**Files:** Modify `src/lite/lite_config_store.{h,cpp}`.

- [ ] Add `LiteTempThrottleConfig {bool enabled; int setpoint;}` + `load/save` (per-key ints `temp_throttle_enabled`, `temp_throttle_setpoint`; defaults false/65, mirror MQTT/RFID accessor pattern).

## Task 3: Temp-throttle web glue

**Files:** Modify `src/lite/web_server_lite.cpp`.

- [ ] Include header; static `s_ttCfg`, `s_ttState{0,0}`, `s_ttLastMs`.
- [ ] `begin()`: `lite_config_load_temp_throttle(s_ttCfg)`.
- [ ] `config_json`: emit `temp_throttle_enabled`, `temp_throttle_setpoint`. `handle_config` POST: accept both; on change save + cache; if disabled release claim + reset state.
- [ ] `build_status_json` / status doc: `temp_throttle` (enabled), `throttling` (`clientHasClaim(TempThrottle)`).
- [ ] `web_server_lite_loop`: every 30 s call tick with `getMinCurrent()`, `isTemperatureValid()`, `getTemperature()`, `isCharging()`, `getChargeCurrent()`; Claim→`claim(TempThrottle, Priority_Safety, {cc})`, Release→`release(TempThrottle)`.
- [ ] `pio run -e openevse_lite` → SUCCESS.
- [ ] Commit `feat(lite): temperature throttle (derate charge current at setpoint)`.

## Task 4: Limit pure unit + native test

**Files:** Create `src/lite/lite_limit.{h,cpp}`, `test/test_lite_limit/test_main.cpp`; Modify `platformio.ini`.

- [ ] Write failing doctest: type-string round-trip (n/t/e/s/r); Time min boundary; Energy `>=`; Soc/Range `>=` AND unavailable(`<0`)→false; None→false.
- [ ] Add `+<lite/lite_limit.cpp>` to native filter; `pio test -e native` → FAIL.
- [ ] Implement `LiteLimitType` helpers + `lite_limit_reached` per spec.
- [ ] `pio test -e native` → PASS.

## Task 5: Limit config store

**Files:** Modify `src/lite/lite_config_store.{h,cpp}`.

- [ ] Add `load/save` for `limit_default_type` (str) + `limit_default_value` (int); `load` returns false when absent.

## Task 6: Limit web glue

**Files:** Modify `src/lite/web_server_lite.cpp`.

- [ ] Include header; static `s_limit{None,0,true}`, `s_limitVersion`, `s_limitWasCharging`, `s_limitLastMs`.
- [ ] Hand-rolled `limit_parse(body,len,&props)` (type from string, value, auto_release default true; valid only if type!=None && value>0) + `limit_get_json(out)`.
- [ ] `begin()`: seed from `limit_default_*` (auto_release=false) if present.
- [ ] `handle_limit`: GET serialize; POST set+bump version (201/400); DELETE clear. Route `s_server.on("/limit", handle_limit)`.
- [ ] `web_server_lite_loop` (~1 s gate): charge→idle falling edge → release claim + (if auto_release) clear; if `hasLimit` && charging && `lite_limit_reached(...,getSessionElapsed,getSessionWattHours,-1,-1)` → `claim(Limit, Priority_Limit, {Disabled, autoRelease=true})`; else (no limit) release if held.
- [ ] `build_status_json`: `limit` (hasLimit bool) + `limit_version`.
- [ ] `pio run -e openevse_lite` → SUCCESS.
- [ ] Commit `feat(lite): session limits (/limit time/energy; soc/range inert until vehicle data)`.

## Task 7: Final verify

- [ ] `pio test -e native` full suite green (24 suites).
- [ ] `pio run -e openevse_lite` green; note flash %.
- [ ] Update memory (parity status: temp-throttle + limit done; SoC/Range pending vehicle_extras).
