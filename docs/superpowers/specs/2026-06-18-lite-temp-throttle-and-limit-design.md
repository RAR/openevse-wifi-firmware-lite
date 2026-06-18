# Lite Temp-Throttle & Session-Limit — Design

**Goal:** Bring two standard-firmware features to JuiceBox-lite: **temperature throttling** (derate charge current as the unit heats) and **session limits** (`/limit` — stop charging after a time/energy/SoC/range threshold). Both are "gated lifts" onto the Slice-1.5 `LiteEvseManager` claim seam.

**Why now:** Both were the cheapest remaining standard-fw parity gaps. The control seam already exposes every getter and claim ID they need; the gui-nightshift UI already has both pages, so this is backend-only.

## Shared architecture

Follow the established lite pattern (divert/shaper/override):
- **Pure decision function** lives in an unguarded `lite_<feature>.{h,cpp}`, added to the `[env:native]` `build_src_filter`, covered by a `test/test_lite_<feature>/` doctest suite.
- **Runtime glue** (config cache, `/config` + `/status` fields, route handlers, per-tick claim/release) lives inline in `web_server_lite.cpp`, guarded by that file's `OPENEVSE_LITE`.
- **Config** persists via `lite_config_store` per-key blobs, mirroring upstream `app_config` key names.
- Claims go through `s_mgr_ctrl->claim/release/clientHasClaim` with `EvseProperties`. All claim IDs/priorities already exist in `lite_evse_claims.h`.

## Feature 1 — Temperature throttle

Transcribed from `src/temp_throttle.cpp` (standard fw). Engages only while charging once `temp >= setpoint`, ramping the commanded current down 1 A per loop toward the J1772 floor, recovering 1 A per loop once cool, releasing the claim when fully recovered.

**Temp source confirmed:** `juicebox_proto.cpp` parses a `'T'` field into `_status.temp`; `LiteEvseManager::getTemperature()` / `isTemperatureValid()` are wired. (HW caveat: confirm the `'T'` value is sane °C on the bench before trusting a 65 °C setpoint.)

**Pure unit** `lite_temp_throttle.{h,cpp}`:
```cpp
struct LiteTempThrottleCfg   { int setpoint_c; int min_current_a; };
struct LiteTempThrottleState { uint32_t start_current; uint32_t throttled_current; }; // start==0 => idle
enum class LiteTempThrottleAction : uint8_t { None, Claim, Release };
struct LiteTempThrottleResult { LiteTempThrottleAction action; uint32_t charge_current; };

LiteTempThrottleResult lite_temp_throttle_tick(
    const LiteTempThrottleCfg &cfg, LiteTempThrottleState &st,
    bool enabled, bool temp_valid, int temp_c, bool charging, uint32_t pilot);
```
Logic: disabled → reset state, `Release` if it had been throttling else `None`. Invalid temp → `None`. `temp>=setpoint` → engage (only if charging && pilot>0), decrement throttled toward `min_current_a`, return `Claim(throttled)`. `temp<setpoint` while throttling → increment toward start; `Claim` until recovered, then reset + `Release`.

**Glue (web_server_lite.cpp):** `LiteTempThrottleConfig {bool enabled; int setpoint;}` (default `false`/65). Ticked every 30 s in `web_server_lite_loop()`. `Claim` → `claim(EvseClient_OpenEVSE_TempThrottle, EvseManager_Priority_Safety, {chargeCurrent})`; `Release` → `release(...)`. `min_current_a = getMinCurrent()`. `/config` GET+POST `temp_throttle_enabled`, `temp_throttle_setpoint`; disabling releases the claim + resets state. `/status` exposes `temp_throttle` (enabled) + `throttling` (`clientHasClaim`).

## Feature 2 — Session limit

Transcribed from `src/limit.cpp` + `/limit` route. A persisted-default + runtime-settable typed limit; when charging and the threshold is hit, claim `Disabled` (auto-release) at `Priority_Limit`; clear/release on session end.

**Pure unit** `lite_limit.{h,cpp}`:
```cpp
enum class LiteLimitType : uint8_t { None=0, Time, Energy, Soc, Range };
LiteLimitType lite_limit_type_from_string(const char *s);   // first-char match, upstream-compatible
const char   *lite_limit_type_to_string(LiteLimitType t);
struct LiteLimitProps { LiteLimitType type; uint32_t value; bool auto_release; };
bool lite_limit_reached(const LiteLimitProps &p, uint32_t sessionElapsedS,
                        uint32_t sessionEnergyWh, int soc, int rangeKm);
```
`Time`: `value>0 && elapsedS/60 >= value`. `Energy`: `value>0 && energyWh >= value`. `Soc`/`Range`: `value>0 && metric >= value`, but **metric<0 (unavailable) → false** — so SoC/Range are accepted but inert until the vehicle_extras gap closes.

**Glue (web_server_lite.cpp):** `LiteLimitProps s_limit{None,0,true}` + `s_limitVersion`. Hand-rolled ArduinoJson parse/serialize (lite `EvseProperties` has no `JsonSerialize` template). Routes: `GET /limit` (serialize current, `{}` when none), `POST /limit` (set; requires `type!=None && value>0`), `DELETE /limit` (clear). Loop enforcement (~1 s gate): when charging + `hasLimit` + `lite_limit_reached` → `claim(EvseClient_OpenEVSE_Limit, EvseManager_Priority_Limit, {Disabled, autoRelease})`; no limit → release if held. Session end detected via the same charge→idle falling-edge the override glue uses (`s_limitWasCharging`); on that edge release the claim and, if `auto_release`, clear the limit. Default seeded at begin from `limit_default_type`/`limit_default_value` (auto_release=false), mirroring upstream `setDefaultLimit`.

**Deliberate simplification:** upstream has an "auto-enable when default state is Disabled" branch; lite's base target is Active by default, so that branch is omitted (YAGNI). SoC/Range inert as above. No `event_send` push (lite has no `/ws` limit_version channel today).

## Testing

Native doctest suites for both pure units: temp-throttle ramp engage/decrement/floor/recover/release + disabled-reset + invalid-temp; limit type-string round-trip + each predicate boundary (`>=`) + SoC/Range-unavailable→false. Device build (`pio run -e openevse_lite`) green. HW validation (throttle ramp under a warm unit; limit stop on a real session) is the user's bench — deferred, folded into task #117.

## Footprint

No fork dependency, no new partition. ~2–3 KB flash each on top of the current 761,480 B (77.5 % of the 960 KB OTA slot).
