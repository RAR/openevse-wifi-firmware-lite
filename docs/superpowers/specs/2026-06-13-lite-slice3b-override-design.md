# Slice 3b: OpenEVSE-compatible `/override` control endpoint — Design

**Date:** 2026-06-13
**Branch:** `feature/juicebox-lite`
**Worktree:** `/home/rar/oevse/openevse-juicebox-lite`

## Context

Slice 3a shipped a read-only OpenEVSE-shaped `GET /status`, so the firstof9/openevse
Home Assistant integration can *observe* a JuiceBox-lite unit. Slice 3b adds the
**write** half: the `/override` endpoint the integration (and `evcc`, and the OpenEVSE
app) uses to *drive* the EVSE — start/stop charging and set the charge current.

The Slice 1.5 control seam is already in place: `ManualOverride` → `LiteEvseManager`
claim registry → pure arbitration → `JuiceBoxBackend`. A manual claim with
`state=Active` + `charge_current=N` already resolves to the backend's `AL00N` keepalive;
`state=Disabled` resolves to the `AL...:00` J1772 stop. What's missing is the *HTTP
contract* on top of that seam: today's `/override` is a query-param stub
(`?state=active|disabled|release`) with no JSON body, no `charge_current`, no DELETE, and
no proper GET echo.

The user chose **full override semantics** during brainstorming: `state` +
`charge_current` + `max_current` + `auto_release` + `energy_limit` + `time_limit`.

## Goal

A method-routed `/override` endpoint matching the OpenEVSE local-API contract, so the HA
integration can start/stop charging, set the charge current, and impose session
energy/time limits — all routed through the existing manager seam to the JuiceBox `AL`
keepalive.

## OpenEVSE contract (what we mirror)

The standard firmware routes `/override` by HTTP method:

| Method   | Behavior |
|----------|----------|
| `GET`    | If a manual override is active, return its properties object; else `{}`. |
| `POST`   | Parse a JSON body into override properties, `claim()` them. `201 Created`. |
| `DELETE` | `release()` the manual override. `200`. |
| `PATCH`  | `toggle()` the manual override. `200`. |

The standard `EvseProperties` JSON carries `state` (string), `charge_current`,
`max_current`, `auto_release`. State strings: `"active"`, `"disabled"`, `"clear"`.
NOTE: the lite `EvseState` is a plain `enum class { None, Active, Disabled }` — it has
**no** `fromString`/`toString`. String↔enum mapping is done by hand in the glue (as the
existing `/override` stub already does with `strcmp`): `"active"`→`Active`,
`"disabled"`→`Disabled`, `"clear"`→`None`; and for GET echo `Active`→`"active"`,
`Disabled`→`"disabled"`, `None`→omit `state`.

## Design decisions (documented per the autonomy mandate)

**D1 — `energy_limit`/`time_limit` are a lite-side extension, NOT added to
`EvseProperties`.** This fork's base `EvseProperties` (and therefore the lite shim)
carries only state/charge_current/max_current/auto_release; the upstream HA integration
*does* send `energy_limit`/`time_limit`, so to be faithful we honor them — but we keep the
`EvseProperties` shim byte-faithful to the fork and track the two limits in a *separate*
lite-owned `LiteOverrideLimits` struct held by `web_server_lite`. Limits are **session-
relative** (compared against `LiteSessionEnergy` Wh / elapsed — no NTP/wall-clock needed;
the clock from Slice 3-clock is for time-of-day, a different axis).

**D2 — limit expiry makes the override a sticky Disable, never a resume.** When a session
energy or time limit is exceeded, the override is re-asserted as `state=Disabled` (stops
charging) and marked *expired*. An expired/Disabled override is **sticky**: it is never
auto-released, so the EV stays stopped until the user clears it (`DELETE`) or POSTs a new
override. This is the safe choice on hardware we cannot yet validate (the bench unit
hard-faults on GFI) — there is no code path that can spontaneously *resume* charging.

**D3 — `auto_release` enforced conservatively, for Active overrides only.** `auto_release`
is currently stored but enforced nowhere. We enforce it as: an **Active**
(charging-enabling) override with `auto_release=true` is released on the charging→idle
falling edge (one-shot "start charging now" that doesn't linger). **Disabled** and
**expired** overrides are sticky (D2) — never auto-released — so a stop can never silently
become a resume. `auto_release` defaults to `true` (matching `ManualOverride::claim`).

**D4 — the testable logic is a pure unit; HTTP/JSON and manager calls are thin glue.** The
decision "given the override's limits, the live session Wh/elapsed, the override state, and
the charging edge, what should happen — keep / stop / release?" is pure and lives in a
native-tested `lite_override` unit. JSON field extraction and `manual.claim/release` calls
stay in `web_server_lite.cpp` as glue (mirroring how `/status` and `/config` JSON is glue),
because field extraction is mechanical and the manager seam is already covered by Slice 1.5
tests.

## Architecture

### New pure unit: `src/lite/lite_override.{h,cpp}`

```cpp
#include <stdint.h>

// Session-relative override limits (lite extension to EvseProperties). A limit of 0 /
// has_* == false means "no limit on this axis".
struct LiteOverrideLimits {
  uint32_t energy_limit_wh = 0;   // stop when session Wh >= this
  uint32_t time_limit_s    = 0;   // stop when session elapsed s >= this
  bool     has_energy      = false;
  bool     has_time        = false;
};

enum class LiteOverrideAction : uint8_t {
  None,    // leave the override as-is
  Stop,    // a session limit was exceeded -> re-assert Disabled (sticky), mark expired
  Release  // an Active auto-release override hit the charge->idle edge -> release it
};

// Pure decision. Inputs:
//   limits          - the active override's session limits
//   sessionWh       - LiteSessionEnergy::wattHours() (session-relative, resets per plug-in)
//   sessionElapsedS - LiteSessionEnergy::elapsedSecs()
//   overrideActive  - is a manual override currently claimed?
//   overrideEnabling- does the override resolve to a charging-enabling (Active) state?
//   autoRelease     - the override's auto_release flag
//   chargingFalling - true on the loop iteration where isCharging() went true -> false
// Order of precedence: a limit Stop wins over an auto-release Release (so a limit on an
// Active auto-release override stops-and-sticks rather than releasing-and-resuming).
LiteOverrideAction lite_override_evaluate(const LiteOverrideLimits &limits,
                                          uint32_t sessionWh, uint32_t sessionElapsedS,
                                          bool overrideActive, bool overrideEnabling,
                                          bool autoRelease, bool chargingFalling);
```

`lite_override_evaluate` returns:
- `Stop` if `overrideActive && (has_energy && sessionWh >= energy_limit_wh) || (has_time &&
  sessionElapsedS >= time_limit_s)`.
- else `Release` if `overrideActive && overrideEnabling && autoRelease && chargingFalling`.
- else `None`.

No `OPENEVSE_LITE` guard (compiles native). `.cpp` added to `[env:native]`
`build_src_filter`.

### Modified: `src/lite/web_server_lite.cpp`

**Method-routed `/override`** (replace the query-param stub). Branch on `hm->method`
(`mg_vcmp(&hm->method, "GET"|"POST"|"DELETE"|"PATCH")`):

- **POST** — parse `hm->body` as JSON into an `EvseProperties` (`state`/`charge_current`/
  `max_current`/`auto_release`) **and** a `LiteOverrideLimits` (`energy_limit`→Wh,
  `time_limit`→s). `manual.claim(props)`; store the limits + clear the *expired* flag +
  capture whether the override is enabling. Respond `201 {"msg":"Created"}` (or `500` on
  claim failure, `400` on unparseable JSON). Reuse the existing 10 s idle-connection reaper
  for bodyless-POST safety.
- **GET** — if `manual.isActive()`, serialize the active override: `state` (string),
  `charge_current`, `max_current` (omitted when unset/`UINT32_MAX`), `auto_release`, plus
  `energy_limit`/`time_limit` when set, and `expired` (bool). Else `{}`.
- **DELETE** — `manual.release()`; clear stored limits + expired flag. `200 {"msg":"Deleted"}`.
- **PATCH** — `manual.toggle()`. `200 {"msg":"Updated"}`.

Keep accepting the legacy `?state=` query form on GET-with-query as a bodyless convenience
(same rationale as `/config`): if a `state` query var is present it is treated as a POST-
equivalent claim/release. This preserves any existing query-param callers.

**Enforcement in `web_server_lite_loop()`** (it already holds `s_mgr_ctrl` + sees `manual`):
each iteration, detect the charging falling edge (track a static `s_wasCharging` from
`s_mgr_ctrl->isCharging()`), then call `lite_override_evaluate(...)` with the stored limits
and live `s_mgr_ctrl->getSessionWattHours()/getSessionElapsed()`. On:
- `Stop`  → `manual.claim(EvseProperties(EvseState::Disabled))`; set `expired = true`.
- `Release` → `manual.release()`; clear limits + expired.
- `None`  → nothing.

`overrideEnabling` = the stored override resolved to `EvseState::Active`; recomputed at
POST and after a `Stop`.

### State held by `web_server_lite.cpp` (file-static)

```cpp
static LiteOverrideLimits s_ovrLimits;   // limits of the active override (zeroed when none)
static bool               s_ovrExpired   = false; // a limit fired -> sticky Disable
static bool               s_ovrEnabling  = false; // override resolves to Active
static bool               s_wasCharging  = false; // for the charge->idle falling edge
```

### No new config / persistence

Overrides are volatile (they reset on reboot, matching OpenEVSE — a manual override is not
persisted). No `lite_config_store` changes.

## Data flow

```
POST /override {state, charge_current, energy_limit, ...}
   -> parse -> EvseProperties + LiteOverrideLimits
   -> manual.claim(props)  -> LiteEvseManager.apply() -> arbitrate -> JuiceBox AL keepalive
   -> store s_ovrLimits, s_ovrEnabling, s_ovrExpired=false

web_server_lite_loop() each tick:
   sessionWh/elapsed (LiteSessionEnergy) + charging edge
   -> lite_override_evaluate(...)
        Stop    -> manual.claim(Disabled), s_ovrExpired=true   (sticky)
        Release -> manual.release(), clear limits
GET /override -> serialize active override (+ energy/time/expired)
DELETE /override -> manual.release(), clear limits
```

## Error handling

- **Unparseable POST body** → `400`, no claim mutated.
- **Claim registry full** (8 claims) → `claim()` returns false → `500`.
- **Bodyless POST** (no Content-Length) → the existing 10 s idle reaper closes the wedged
  connection; the `?state=` query fallback lets bodyless clients still drive it.
- **Limit set but clock/NTP absent** → irrelevant; limits are session-relative
  (`LiteSessionEnergy`), independent of wall-clock.
- **`max_current` above hardware** → already clamped in `LiteEvseManager::apply()`
  (`lite_clamp_charge_current`), no new clamp needed.

## Testing

### Native (doctest) — `test/test_lite_override/`

- `lite_override_evaluate`: energy limit reached → `Stop`; time limit reached → `Stop`; both
  unset → never `Stop`; below both limits → `None`; Active + auto_release + charge→idle edge
  → `Release`; Disabled override + edge → `None` (sticky, never released); limit + auto_release
  both true at the edge → `Stop` wins (precedence); `overrideActive==false` → always `None`;
  exact-boundary (`sessionWh == energy_limit_wh`) → `Stop` (>= semantics).

### On-device (DEFERRED — bench can't charge)

The bench JuiceBox 40 is an incomplete unit (no GFCI → hard-faults on GFI auto-test →
halts), so charge-control and limit-expiry cannot be live-validated. Code-complete +
native-tested only; live validation defers to a complete unit. The HTTP routing itself
(GET/POST/DELETE returning the right shapes) is observable on the bench over WiFi but the
*effect* on charging is not. Treat the RE control mapping as ground truth.

## Files

- **Create:** `src/lite/lite_override.{h,cpp}`, `test/test_lite_override/`
- **Modify:** `src/lite/web_server_lite.cpp` (method-routed `/override` + loop enforcement)
- **Modify:** `platformio.ini` (`+<lite/lite_override.cpp>` on `[env:native]` build_src_filter)
- **Bump:** `LITE_FW_VERSION` `"lite-3a"` → `"lite-3b"`
