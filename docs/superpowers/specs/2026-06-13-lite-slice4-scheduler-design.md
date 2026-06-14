# Slice 4: Time-of-Day Charging Scheduler — Design

**Date:** 2026-06-13
**Branch:** `feature/juicebox-lite`
**Worktree:** `/home/rar/oevse/openevse-juicebox-lite`

## Context

The Slice-3 clock shipped a synced wall-clock (`LiteClock`, local-time aware). That
unblocks a **time-of-day charging scheduler**: rules like "enable charging at 23:00 on
weeknights, disable at 06:00" — the classic off-peak-charging use case. OpenEVSE exposes
this via a `/schedule` endpoint and a `Scheduler` task that claims the EVSE state at event
boundaries; the HA integration and the OpenEVSE app both drive it.

The standard firmware's `Scheduler` is ~1200 lines (a `MicroTasks::Task` with event
instances, start-time randomisation, a forward-looking "plan", and per-event repeat flags).
That is far too heavy for the 2 MB lite flash budget and most of it is unused on a JuiceBox.
This slice builds a **trimmed** scheduler that mirrors the OpenEVSE `/schedule` JSON
contract but with a small pure evaluation core.

## Goal

A persistent weekly charging schedule: OpenEVSE-shaped `/schedule` events (`{id, state,
time, days}`) that, evaluated against the local wall-clock, claim charging-enabled or
charging-disabled through the existing manager seam — below a manual override in priority.

## OpenEVSE `/schedule` contract (what we mirror)

Each event is `{ "id": <int>, "state": "active"|"disabled", "time": "HH:MM:SS",
"days": ["monday","tuesday",...] }`. The fork's events are **state-only** (the
deserialize checks `state`+`time`+`days`; no per-event charge_current). The active state
at any instant is the state of the **most-recently-fired** event, looking back across the
week. Routes: `GET /schedule` (all events), `POST /schedule` (add/update by id),
`DELETE /schedule/<id>` (remove one).

## Design decisions (documented per the autonomy mandate)

**D1 — state-only weekly events; drop randomisation, one-shot, and per-event current.** The
fork's events are already state-only, so this is faithful, not a reduction. All events
repeat weekly (the implicit `SCHEDULER_REPEAT`); there are no one-shot events and no
start-time randomisation (a solar/load-balancing feature a JuiceBox-lite doesn't need).
YAGNI.

**D2 — bounded event table (16).** The standard cap is 50; a weekly schedule rarely needs
more than ~2 events/day. 16 keeps the persisted blob tiny (size-asserted, like
`LiteEnergyTotals`) and the eval trivially cheap. Document the cap; `POST` past it → 507.

**D3 — pure week-relative eval.** "Most-recently-fired event" is computed as: for every set
day-bit of every event, the event's fire-offset within the notional week is `day*86400 +
sec_of_day`; `ago = (now_offset + WEEK − fire_offset) mod WEEK`; the event with the
smallest `ago` wins, and its state is the active state. This naturally handles the 7-day
lookback and the week wraparound (e.g. now=Sunday 05:00, controlling event=Saturday 23:00).
No events / no set days → `None` (no claim). Fully native-testable.

**D4 — schedule claims the `Schedule` client at `Priority_Timer` (100).** Below
`Priority_Manual` (1000) so a manual `/override` always wins; above base config
(`Priority_Default` 10) so the schedule gates the default. Active→claim Active,
Disabled→claim Disabled, None→release the claim. Re-claim only when the resolved schedule
state changes (avoid churning the keepalive every loop).

**D5 — schedule disabled when the clock is unsynced.** Time-of-day scheduling needs a valid
wall-clock; while `!clock.valid()` the schedule claim is released (EVSE follows base/manual).
This is unlike the session-relative override limits (Slice 3b), which need no clock.

**D6 — events ARE persisted** (a weekly schedule must survive reboot), unlike the volatile
override. One FlashDB blob, mirroring the `lite_config_*_totals` pattern.

**D7 — testable core vs glue.** The eval, the `HH:MM[:SS]`/day-name parsing+formatting, and
the upsert/remove table ops are pure and native-tested in `lite_schedule`. The JSON
(de)serialization, persistence, and manager-claim wiring stay as thin glue in
`web_server_lite.cpp` (matching the `/status`/`/config`/`/override` pattern).

## Architecture

### New pure unit: `src/lite/lite_schedule.{h,cpp}`

```cpp
#include <stdint.h>
#include <stddef.h>

#define LITE_SCHEDULE_MAX_EVENTS 16

// One weekly schedule event. state uses EvseState values (1=Active, 2=Disabled); id 0 is
// reserved "empty". 12 bytes, fixed layout for the persisted blob.
struct LiteScheduleEvent {
  uint32_t id;          // client-assigned, nonzero
  uint8_t  day_mask;    // bit0=Sun .. bit6=Sat
  uint8_t  state;       // 1=Active, 2=Disabled
  uint8_t  pad[2];      // zero; keeps the struct 4-byte aligned and blob-stable
  uint32_t sec_of_day;  // 0..86399 fire time (local)
};

struct LiteSchedule {
  LiteScheduleEvent events[LITE_SCHEDULE_MAX_EVENTS];
  uint32_t count;       // populated slots [0..MAX]
};
// sizeof == 16*12 + 4 == 196 (size-asserted in the .cpp).

// Most-recently-fired event's state as of (dayOfWeek 0=Sun..6=Sat, secOfDay 0..86399).
// Returns 0 (EvseState::None) when no event applies. Pure.
uint8_t lite_schedule_active_state(const LiteSchedule &s, int dayOfWeek, uint32_t secOfDay);

// Upsert by id (replace same-id, else append). False if full and id not present. Pure.
bool lite_schedule_upsert(LiteSchedule &s, const LiteScheduleEvent &e);
// Remove by id. False if absent. Pure. Compacts the array (order not preserved).
bool lite_schedule_remove(LiteSchedule &s, uint32_t id);

// "HH:MM" or "HH:MM:SS" -> seconds of day. False on malformed/out-of-range. Pure.
bool lite_schedule_parse_time(const char *hhmmss, uint32_t &secOfDay);
// secOfDay -> "HH:MM:SS" (cap >= 9). Pure.
void lite_schedule_format_time(uint32_t secOfDay, char *buf, size_t cap);
// day name (lowercase, e.g. "monday") -> bit index 0..6 (Sun=0); -1 unknown. Pure.
int  lite_schedule_day_index(const char *name);
// bit index 0..6 -> lowercase day name (static string); nullptr if out of range. Pure.
const char *lite_schedule_day_name(int index);
```

Eval (the heart):

```cpp
uint8_t lite_schedule_active_state(const LiteSchedule &s, int dayOfWeek, uint32_t secOfDay) {
  const uint32_t WEEK = 7u * 86400u;
  uint32_t now = (uint32_t)dayOfWeek * 86400u + secOfDay;
  uint32_t bestAgo = WEEK; uint8_t bestState = 0;
  for (uint32_t i = 0; i < s.count; i++) {
    const LiteScheduleEvent &e = s.events[i];
    if (e.id == 0) continue;
    for (int d = 0; d < 7; d++) {
      if (!(e.day_mask & (1u << d))) continue;
      uint32_t fire = (uint32_t)d * 86400u + e.sec_of_day;
      uint32_t ago  = (now + WEEK - fire) % WEEK;
      if (ago < bestAgo) { bestAgo = ago; bestState = e.state; }
    }
  }
  return bestState;
}
```

No `OPENEVSE_LITE` guard (compiles native); `.cpp` added to `[env:native]` build filter.

### Modified: `src/lite/lite_config_store.{h,cpp}`

Add `lite_config_load_schedule(LiteSchedule&)` / `lite_config_save_schedule(const
LiteSchedule&)` — one FlashDB blob key (`"sched"`), mirroring `load/save_totals`. Load
returns false when the key is absent (caller zero-inits an empty schedule).

### Modified: `src/lite/web_server_lite.cpp`

- Own a file-static `LiteSchedule s_schedule` + `uint32_t s_scheduleVersion` + a
  `uint8_t s_lastSchedState` last-resolved-state cache. Load the schedule at
  `web_server_lite_begin` (`if (!lite_config_load_schedule(s_schedule)) s_schedule = {}`).
- **`GET /schedule`** → serialize a JSON array of `{id, state, time, days}` from
  `s_schedule` (state via "active"/"disabled"; time via `lite_schedule_format_time`; days
  via `lite_schedule_day_name` over the mask).
- **`POST /schedule`** → parse `{id, state, time, days}`; build a `LiteScheduleEvent`
  (`lite_schedule_parse_time`, day names→mask via `lite_schedule_day_index`); `upsert`;
  persist; bump `s_scheduleVersion`. `201` (or `507` if the table is full, `400` on bad JSON
  / missing required fields).
- **`DELETE /schedule/<id>`** (and `DELETE /schedule?id=<id>`) → `remove`; persist; bump
  version; `200`/`404`. (URI-suffix parse like the standard `SCHEDULE_PATH_LEN`.)
- Emit `schedule_version` in `/status` (so HA can detect changes), set to
  `s_scheduleVersion`.
- **Eval wiring in `web_server_lite_loop`** (after the override-enforcement block): if
  `s_clock && s_clock->valid()`, compute `local = s_clock->nowLocal(millis())`,
  `dow = (int)(((local / 86400u) + 4u) % 7u)` (1970-01-01 = Thursday; Sun=0),
  `sod = local % 86400u`; `uint8_t st = lite_schedule_active_state(s_schedule, dow, sod)`.
  If `st != s_lastSchedState`: apply via the manager — `st==1`→`claim(Schedule,
  Priority_Timer, Active)`, `st==2`→`claim(... Disabled)`, `st==0`→`release(Schedule)` —
  and store `s_lastSchedState = st`. If `!valid()` and `s_lastSchedState != 0`, release and
  reset to 0.

`LiteEvseManager` already exposes `claim(EvseClient, int, EvseProperties&)` and
`release(EvseClient)`; `EvseClient_OpenEVSE_Schedule` (0x00010004) and
`EvseManager_Priority_Timer` (100) already exist in `lite_evse_claims.h`.

### `platformio.ini`

Add `+<lite/lite_schedule.cpp>` to `[env:native]` `build_src_filter`.

## Data flow

```
POST /schedule {id,state,time,days} -> parse -> LiteScheduleEvent -> upsert(s_schedule)
   -> save_schedule (FlashDB) -> s_scheduleVersion++
web_server_lite_loop (clock valid):
   nowLocal -> (dayOfWeek, secOfDay) -> lite_schedule_active_state
      Active   -> manager.claim(Schedule, Timer, Active)
      Disabled -> manager.claim(Schedule, Timer, Disabled)
      None     -> manager.release(Schedule)        (only on change)
   -> arbitrate (Manual override outranks) -> JuiceBox AL keepalive
GET /schedule -> array of {id,state,time,days}
DELETE /schedule/<id> -> remove -> save -> version++
```

## Error handling

- **Clock unsynced** → schedule released (D5); no time basis.
- **Malformed time / unknown day / missing field on POST** → `400`, table untouched.
- **Table full on POST of a new id** → `507 Insufficient Storage`, table untouched.
- **DELETE unknown id** → `404`.
- **Persist failure** → apply in-RAM anyway (best effort), `503` on the mutating response
  (mirrors `/config`).
- **Manual override active** → arbitration makes Manual (1000) win over Schedule (100); the
  schedule claim still tracks underneath and resumes when the override is released.

## Testing

### Native (doctest) — `test/test_lite_schedule/`

- `parse_time`: "23:00"→82800, "06:30:15"→23415, "24:00" / "12:60" / "9x" → false.
- `format_time`: 82800→"23:00:00", 0→"00:00:00".
- `day_index`/`day_name`: "sunday"→0, "saturday"→6, "funday"→-1; round-trip 0..6.
- `upsert`: append new; replace same-id (count unchanged); fill to 16 then 17th new → false;
  replacing an existing id when full → true.
- `remove`: removes by id, count drops, compacts; unknown id → false.
- `active_state`:
  - empty schedule → 0.
  - single Active event Mon 23:00, now Mon 23:30 → Active; now Mon 22:30 → looks back to the
    most recent prior event (none earlier → 0 if single event hasn't fired this week... see
    wrap case).
  - wrap: Active Sat 23:00 only, now Sun 05:00 → Active (fired 6h ago).
  - two events same day (06:00 Disabled, 23:00 Active): now 12:00 → Disabled; now 23:30 →
    Active; now 05:00 → Active (last night's 23:00 is most recent across the wrap).
  - multi-day mask: event on Mon|Wed|Fri 08:00, now Wed 09:00 → that state.
  - precedence at exact tie: two events firing the same instant → last-evaluated wins
    (document; not a real-world concern).

### On-device (PARTIAL — bench can't charge)

The `/schedule` CRUD + `schedule_version` + the *resolved claim state* (observable via
`/status` `state`/`status` while no vehicle) are bench-checkable over WiFi once the clock
syncs. The *effect on actual charging* is DEFERRED (bench hard-faults on GFI). Clock-driven
claim transitions can be smoke-tested by setting an event a minute ahead and watching the
`/status` state flip at the boundary.

## Files

- **Create:** `src/lite/lite_schedule.{h,cpp}`, `test/test_lite_schedule/`
- **Modify:** `src/lite/lite_config_store.{h,cpp}` (schedule blob persistence)
- **Modify:** `src/lite/web_server_lite.cpp` (`/schedule` routes + loop eval/claim +
  `schedule_version` in `/status`)
- **Modify:** `platformio.ini` (`+<lite/lite_schedule.cpp>` on `[env:native]`)
- **Bump:** `LITE_FW_VERSION` `"lite-3b"` → `"lite-4"`
