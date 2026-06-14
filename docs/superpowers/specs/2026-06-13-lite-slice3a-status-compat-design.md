# Slice 3a: OpenEVSE `/status` Compatibility — Design

**Date:** 2026-06-13
**Branch:** `feature/juicebox-lite`
**Worktree:** `/home/rar/oevse/openevse-juicebox-lite`

## Context

Slice 3 makes the JuiceBox-lite firmware compatible with the
[firstof9/openevse](https://github.com/firstof9/openevse) Home Assistant
integration, which talks to the unit over the **local HTTP API** (not MQTT).
The integration's `python-openevse-http` library:

- Polls `GET /status` every **60 s** via an `OpenEVSEUpdateCoordinator`
  (`manager.update()`). Setup fails only if that poll fails
  (`ConfigEntryNotReady`).
- Optionally opens a `/ws` websocket for real-time push — **not required**;
  connection errors are caught and logged, setup continues. (Websocket push is
  sub-slice 3d.)
- Reads ~74 distinct `/status` keys via `properties.py`, all with `.get()`
  defaults — missing keys degrade to `None`/unavailable, they do not crash.

Slice 3 decomposes into: **3a** richer `GET /status` (this doc) → **3b**
`/override` control → **3c** HTTP push-in → **3d** `/ws` push.

3a is the foundation: it locks the status contract that 3b's response and 3c's
pushed fields echo, and that 3d re-broadcasts verbatim.

## Goal

Emit a `GET /status` body that maps real JuiceBox state onto the OpenEVSE key
set the HA integration consumes, including **session-level energy**
(host-side accumulator), so the integration sets up and produces meaningful
entities (state, current, power, session energy, temperature).

## Scope

**In:** state/status mapping, live telemetry passthrough, a host-side session
energy accumulator (Wh + elapsed), derived voltage, identity/system fields.

**Out (deferred):**
- Lifetime totals `total_day/week/month/year` — need a real clock; deferred to
  **3c-prime / after Slice 4 (NTP)**. This is the "C" scope.
- `/ws` websocket push — sub-slice **3d**.
- `/override` control write path — sub-slice **3b**.
- OpenEVSE-only keys (divert, shaper, OCPP, GFCI/ground counts, diode/vent/relay
  temps) — emitted as **absent**; the HA integration tolerates them.

## Architecture

Energy integration is generic math, not JuiceBox-specific, so it lives in its
own pure unit — mirroring the existing `lite_charge_policy` pattern.

### New: `src/lite/lite_session_energy.{h,cpp}`

A pure `LiteSessionEnergy` class — no Arduino, no backend dependency, fully
native-testable.

```cpp
class LiteSessionEnergy {
public:
  // Advance the accumulator. power_w = instantaneous power (W), charging =
  // device actively charging, now_ms = millis()-style monotonic clock.
  // Integrates power over the interval since the last tick (rectangular).
  // A connect->charging edge (charging goes true after being false) starts a
  // fresh session: zeroes session energy + elapsed.
  void tick(int power_w, bool charging, uint32_t now_ms);

  uint32_t wattSeconds() const;   // session watt-seconds (integer)
  uint32_t wattHours()   const;   // session Wh = wattSeconds / 3600
  uint32_t elapsedSecs() const;   // seconds since session start

  void reset();                   // hard reset (e.g. boot)
private:
  // accumulator state: last tick ms, running watt-seconds (double or uint64
  // to avoid overflow), session-start ms, prev charging flag.
};
```

Integration detail: accumulate in watt-seconds using
`power_w * dt_ms / 1000` per tick, carried in a wide accumulator to avoid
overflow over long sessions. `elapsed` counts wall time from session start
while a session is active.

### Owner: `LiteEvseManager` gains `loop()`

`LiteEvseManager::loop()` reads `backend.getPower()` / `backend.isCharging()`
and ticks the accumulator each main-loop iteration. Wired in `main_lite.cpp`
next to `backend.loop()`. The manager remains the status aggregator (it already
owns `addStatusFields` + the delegating getters); the JuiceBox backend stays a
pure transport/decoder.

New manager getters surface the accumulator for `/status`:
`getSessionWattSeconds()`, `getSessionWattHours()`, `getSessionElapsed()`.

### `src/lite/web_server_lite.cpp`

`build_status_json` grows to emit the full key set below. `StaticJsonDocument`
sized up from 512 as needed for the larger object (exact size pinned during
implementation against the populated document).

## `/status` field contract

### State

Map the canonical `LiteEvseState` onto OpenEVSE's `state` int + `status` string:

| LiteEvseState     | `state` | `status`         | notes |
|-------------------|---------|------------------|-------|
| Unknown           | 0       | `"unknown"`      | |
| NotConnected      | 1       | `"not connected"`| |
| Connected         | 2       | `"connected"`    | |
| Charging          | 3       | `"charging"`     | |
| Error             | 8       | `"error"`        | + raw `$WR` text in `fault_text` |
| manual-Disabled   | 254     | `"sleeping"`     | when `manual` claims Disabled |

`Error -> 8` is best-effort: we only know `fault != 0`, not the OpenEVSE fault
taxonomy. The actual JuiceBox fault string (`$WR`, e.g. `"006:GFI Auto Test
Fail"`) rides along in a `fault_text` field so the real cause is visible,
pending hardware fault-code correlation.

### Live telemetry (direct from backend)

| key                 | source |
|---------------------|--------|
| `amp`               | `getAmps()` (reported amps) |
| `pilot`             | `getChargeCurrent()` (advertised setpoint) |
| `power`             | `getPower()` |
| `tempt`, `temp2`    | `getTemperature()` (single sensor → both) |
| `max_current_soft`  | manager soft setpoint |
| `max_current_hard`  | `getMaxHardwareCurrent()` |
| `min_current_hard`  | `getMinCurrent()` (6) |
| `available_current` | arbitrated max (manager) |
| `manual_override`   | `manual.isActive()` (1/0) |
| `mode`              | charge mode string |

### Energy (from accumulator)

| key             | source | unit |
|-----------------|--------|------|
| `wattsec`       | `getSessionWattSeconds()` | watt-seconds |
| `watthour`      | `getSessionWattHours()`   | Wh |
| `session_energy`| `getSessionWattHours()`   | Wh |
| `elapsed`       | `getSessionElapsed()`     | seconds |

Exact key units pinned to `python-openevse-http`'s `properties.py` during
implementation (the library is the source of truth for how each key is scaled).

### Voltage

`voltage = power / amp` when charging and `amp > 0`; else a nominal value. Keeps
HA's V×I ≈ power consistent without a sensor the JuiceBox doesn't report.

### Identity / system

| key         | source |
|-------------|--------|
| `firmware`, `version` | lite firmware version string |
| `protocol`  | `_pv` (JuiceBox protocol version) |
| `ipaddress` | WiFi local IP |
| `ssid`      | connected SSID |
| `srssi`     | WiFi RSSI |
| `uptime`    | `millis()/1000` |
| `freeram`   | `ESPAL.getFreeHeap()` |

OpenEVSE-only keys (`divertmode`, `shaper_*`, `ocpp_connected`,
`gfcicount`/`nogndcount`/`stuckcount`, `diodet`/`groundt`/`relayt`/`ventt`,
`vehicle_*`) are **omitted** — the integration's `.get()` defaults handle them.

## Session semantics

Follows OpenEVSE:
- A session **starts** on the edge into Charging (`charging` false → true).
- `session_energy` / `wattsec` / `watthour` accrue while charging.
- On charge **stop**, values freeze (not zeroed) so HA reads the final session
  total until the next connect.
- A new connect/charge edge **resets** the session.
- All session state resets on reboot (lifetime totals are the deferred Slice-4
  follow-up).

## Testing

### Native (`test/test_lite_session_energy/`)

doctest suite mirroring `test_lite_charge_policy`:
- Accumulation accuracy: known power + interval → expected watt-seconds/Wh.
- `elapsed` tracks wall time from session start.
- Connect→charging edge resets session energy + elapsed.
- Zero-power / not-charging idle does not accrue.
- Stop freezes (does not zero) the session total.
- Wide-accumulator: long high-power session does not overflow.

### On-device

- `curl GET /status` returns the full key set with live values.
- Add the firstof9/openevse integration in Home Assistant against the unit;
  confirm it sets up (60 s poll) and surfaces state, current, power, session
  energy, and temperature entities.

## Files

- **Create:** `src/lite/lite_session_energy.h`, `src/lite/lite_session_energy.cpp`
- **Create:** `test/test_lite_session_energy/` (doctest)
- **Modify:** `src/lite/lite_evse_manager.{h,cpp}` — add `loop()`, accumulator
  member, session getters.
- **Modify:** `src/lite/web_server_lite.cpp` — expand `build_status_json`.
- **Modify:** `src/lite/main_lite.cpp` — tick `manager.loop()`.
