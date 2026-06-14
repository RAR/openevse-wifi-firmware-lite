# Slice: Wall Clock + Lifetime Energy Totals — Design

**Date:** 2026-06-13
**Branch:** `feature/juicebox-lite`
**Worktree:** `/home/rar/oevse/openevse-juicebox-lite`

## Context

The lite firmware has only a monotonic `millis()` clock — no wall-clock time. A real
clock is foundational: it unblocks the time-of-day scheduler, session timestamps, and
(this slice) the lifetime energy totals (`total_day/week/month/year`) that were the
deferred "C" scope of Slice 3a. On the ESP32 the standard firmware gets SNTP + a
`TimeManager` for free from ESP-IDF; on the WGM160P/EFM32 LibreTiny port there are **no**
time facilities (no `gettimeofday`, no lwIP SNTP library), so we build the clock ourselves.

The firstof9/openevse Home Assistant integration polls `GET /status`; the standard
firmware's contract (`energy_meter.cpp`) emits `total_energy`/`total_day`/`total_week`/
`total_month`/`total_year` in **kWh**, plus a `time` field. We mirror that shape.

## Goal

A synced wall-clock on the WGM160P, exposed in `/status`, plus persistent lifetime energy
totals bucketed by local calendar day/week/month/year — so the HA integration shows real
time and cumulative energy.

## Scope

**In:**
- Software wall-clock synced via NTP, with periodic re-sync and a `valid()` gate.
- Time config (`sntp_hostname`, `time_zone` UTC offset), mirroring the standard keys.
- `time` field in `/status` (ISO-8601 UTC).
- Persistent lifetime energy totals (lifetime + day/week/month/year + switch count),
  accrued on session completion, rolled over on local-calendar boundaries.
- `total_energy`/`total_day`/`total_week`/`total_month`/`total_year` (kWh) +
  `total_switches` in `/status`.

**Out (deferred):**
- Time-of-day **charging scheduler** — a separate slice on top of this clock.
- Full **POSIX TZ / DST** rules — `time_zone` is a fixed signed-minute offset (no DST).
- **EFM32 RTCC** hardware RTC / battery-backed time — software clock is sufficient with
  hourly re-sync; RTCC is a later low-power optimization.
- On-device **totals-accrual** HW validation — needs a charging session; the bench unit
  hard-faults on GFI. (Clock/NTP sync IS bench-validatable: it runs over WiFi, not the ATmega.)

## Architecture

Two pure-cored units (no Arduino/network/flash in the core → fully native-testable) plus a
thin integration layer. Mirrors the existing `lite_session_energy` / `lite_charge_policy`
pattern.

### Sync source: Mongoose SNTP (enabled)

MongooseLite already vendors the SNTP client (`mg_sntp_*`); it is only compiled out
(`MG_ENABLE_SNTP 0`). Enable it (`-D MG_ENABLE_SNTP=1`) and drive `mg_sntp_request` from the
existing `mg_mgr_poll` loop. No new packet/UDP code. (Rejected for v1: a hand-rolled lwIP
UDP NTP client — more code, no benefit; EFM32 RTCC — adds emlib complexity, later optimization.)

### New: `src/lite/lite_clock.{h,cpp}`

Pure software wall-clock.

```cpp
class LiteClock {
public:
  // Seed from an NTP/SNTP UTC epoch (seconds). Records epoch + the millis() at sync.
  void setEpoch(uint32_t utc_seconds, uint32_t now_ms);
  bool     valid() const;                       // false until first setEpoch()
  uint32_t nowUtc(uint32_t now_ms) const;       // epoch + (now_ms - ms@sync)/1000, wrap-safe
  uint32_t nowLocal(uint32_t now_ms) const;     // nowUtc + tzOffsetSeconds
  void setTzOffsetMinutes(int minutes);         // local = UTC + offset (no DST)
  bool resyncDue(uint32_t now_ms) const;        // true once >= RESYNC_INTERVAL since last sync
private:
  uint32_t _epochAtSync = 0, _msAtSync = 0;
  int      _tzOffsetMin = 0;
  bool     _valid = false;
};
```

`nowUtc` uses unsigned `millis()` subtraction so a 32-bit wrap (~49.7 d) yields a correct
delta. `resyncDue` triggers a fresh SNTP request hourly; between syncs the clock free-runs on
`millis()` (drift acceptable for hourly re-sync).

### New: `src/lite/lite_energy_totals.{h,cpp}`

Pure calendar-bucketed accumulator. Internally Wh (`uint64_t` to avoid overflow); kWh out.

```cpp
struct LiteEnergyTotals {            // POD; persisted verbatim to KVDB
  uint64_t lifetime_wh, day_wh, week_wh, month_wh, year_wh;
  int32_t  day_id, week_id, month_id, year_id;   // local-calendar period ids, -1 = unset
  uint32_t switches;                              // session count
};

// Add a completed session's energy at local time `localEpoch`. Rolls over any bucket whose
// period id changed (resets it before adding). If `!clockValid`, adds to lifetime only
// (period ids left untouched). Increments `switches`.
void energy_totals_add(LiteEnergyTotals &t, uint32_t sessionWh, uint32_t localEpoch, bool clockValid);

// Period ids from a local epoch (pure civil-date math, days_from_civil):
int32_t energy_period_day(uint32_t localEpoch);    // days since 1970 (local)
int32_t energy_period_week(uint32_t localEpoch);    // Monday-aligned 7-day bucket index
int32_t energy_period_month(uint32_t localEpoch);   // year*12 + (month-1)
int32_t energy_period_year(uint32_t localEpoch);    // civil year
```

Civil conversion uses the standard `days_from_civil`/`civil_from_days` algorithm (pure,
testable). Week is Monday-aligned: `floor((localDay + 3) / 7)` (1970-01-01 was a Thursday).

### Integration glue

- **`main_lite.cpp`**: own a `LiteClock`. Each loop: if `clock.resyncDue()` (or not yet
  valid), issue a Mongoose SNTP request to `sntp_hostname`; on `MG_SNTP_REPLY`, call
  `clock.setEpoch(reply.time, millis())`. Register `manager.onSessionComplete` → read the
  session Wh + `clock.nowLocal()`, call `energy_totals_add(...)`, persist to KVDB.
- **`web_server_lite.cpp`**: emit `time` (ISO-8601 UTC from `clock.nowUtc`, omitted while
  `!valid()`) and the five `total_*` kWh fields + `total_switches` in `/status`; accept
  `time_zone` (signed minutes) + `sntp_hostname` in the `/config` handler.
- **`lite_config_store.{h,cpp}`**: `load/save` for `LiteEnergyTotals` (one KVDB blob key) and
  the two clock config values.
- **`platformio.ini`**: `[env:openevse_lite]` build_flags `+= -D MG_ENABLE_SNTP=1`.

## Data flow

```
SNTP reply ──> LiteClock.setEpoch ──> nowUtc()/nowLocal() ──> /status "time"
                                                  │
manager.onSessionComplete (sessionWh) ────────────┴─> energy_totals_add(totals, wh, nowLocal, valid)
                                                        ├─> rollover stale buckets
                                                        ├─> += wh ; switches++
                                                        └─> persist KVDB ──> /status total_* (kWh)
```

## Error handling

- **No sync yet** (`!valid()`): `/status` omits `time`; sessions completing pre-sync add to
  `lifetime_wh` + `switches` only (no calendar bucket — period ids stay unset).
- **SNTP failure / timeout**: log, keep the existing epoch (free-run on `millis()`), retry at
  the next `resyncDue`. Never block the main loop.
- **`millis()` wrap**: unsigned subtraction in `nowUtc` keeps the delta correct.
- **First sync after pre-sync sessions**: lifetime total already includes them; buckets begin
  cleanly from the first post-sync session. Acceptable.

## Testing

### Native (doctest)

- `test/test_lite_clock/`: `setEpoch`→`nowUtc` advances with `millis`; wrap-around correctness;
  `valid()` gate; tz offset applied to `nowLocal`; `resyncDue` boundary.
- `test/test_lite_energy_totals/`: accrual adds Wh→kWh; day/week/month/year rollover resets the
  right bucket and only that one; pre-sync (`!clockValid`) adds lifetime only; switch counter;
  period-id civil math at known dates (incl. month/year boundaries, Monday-aligned week).

### On-device

- **NTP sync (bench-validatable now):** flash, confirm `/status.time` populates with correct
  UTC within ~seconds of boot, and re-sync after the interval. Runs over WiFi; independent of
  the faulted ATmega.
- **Totals accrual (deferred):** needs a charging session → a complete (non-faulted) unit.

## Files

- **Create:** `src/lite/lite_clock.{h,cpp}`, `src/lite/lite_energy_totals.{h,cpp}`
- **Create:** `test/test_lite_clock/`, `test/test_lite_energy_totals/` (doctest)
- **Modify:** `src/lite/main_lite.cpp` (clock + SNTP sync + session-complete → totals)
- **Modify:** `src/lite/web_server_lite.cpp` (`time` + `total_*` in `/status`; `/config` keys)
- **Modify:** `src/lite/lite_config_store.{h,cpp}` (persist totals + clock config)
- **Modify:** `platformio.ini` (`-D MG_ENABLE_SNTP=1` on `[env:openevse_lite]`)
