# Lite Event Log (`/logs`) — Design

**Goal:** Serve the OpenEVSE event-log API (`/logs`, `/logs/<index>`) on JuiceBox-lite so gui-nightshift's History screen works — a record of EVSE state changes, each snapshotting energy/elapsed/temperature/state/RFID/SoC at the moment it happened.

**Why this shape:** lite has no LittleFS (the standard `EventLog` writes rotating LittleFS files), so storage is a fixed RAM ring. Completed-session history is persisted to the FlashDB `kvs` partition on the session-complete edge only (infrequent → no flash-wear/WDT concern; verified there's ample headroom in the 32 KB partition). No fork change, no TSDB, no flash re-carve.

## Contract (from the served gui)

- `GET /logs` → `{"min":<n>,"max":<n>}` — page-index range. gui treats this as a **non-fatal capability probe** (`FetchData.svelte`): absent ⇒ History tab hidden, nothing breaks.
- `GET /logs/<index>` → JSON array of entries (note the **slash**; routed via `UriBraces("/logs/{}")`). gui accumulates pages, dedups, sorts by `time` desc.
- Entry fields the gui reads: `time` (date-parseable ⇒ **ISO-8601**), `type` (`"information"|"notification"|"warning"`), `evseState` (int code), `energy` (Wh), `temperature` (°C). Full standard shape emitted for completeness: also `managerState` (string), `evseFlags`, `pilot`, `elapsed`, `temperatureMax`, `divertMode`, `shaper`, optional `rfidTag`, optional `soc`.
- `GET /logs/export` (CSV) — **deferred** (gui's export button only; History view itself uses the two endpoints above).

Single RAM block ⇒ `/logs` reports `{min:0,max:0}`; `/logs/0` returns the whole ring (oldest→… ; gui sorts), any other index returns `[]`.

## Pure unit — `lite_event_log.{h,cpp}` (unguarded, native-tested)

```cpp
#define LITE_EVENTLOG_CAPACITY 40   // * sizeof(entry) must stay < ~3.5 KB (one kvs sector)
enum class LiteEventType : uint8_t { Information = 0, Notification = 1, Warning = 2 };

struct LiteEventLogEntry {            // packed POD (~44 B): 40 * 44 ≈ 1.8 KB
  uint32_t epoch;        // UTC seconds at event time
  uint32_t evseFlags;
  uint32_t energyWh;     // session Wh
  uint32_t elapsedS;     // session seconds
  uint8_t  type;         // LiteEventType
  uint8_t  managerState; // EvseState int (0 none / 1 active / 2 disabled)
  uint8_t  evseState;    // OpenEVSE numeric state code
  uint8_t  divertMode;   // 1 normal / 2 eco
  uint8_t  shaper;       // 0/1
  int8_t   soc;          // -1 = unavailable
  int16_t  tempC;        // whole °C (INT16_MIN = invalid/no sensor)
  uint16_t pilotA;       // A
  char     rfid[15];     // UID hex, "" = none
};

class LiteEventLog {
  void   clear();
  void   push(const LiteEventLogEntry &e);  // append; overwrites oldest when full
  size_t count() const;                     // <= capacity
  const LiteEventLogEntry &at(size_t i) const; // oldest..newest, i in [0,count)
  size_t toBlob(void *buf, size_t cap) const;  // [u8 ver=1][u8 count][entries]; bytes written or 0
  bool   fromBlob(const void *buf, size_t len);// validate ver+count; false on mismatch
};
```
Native doctest: wrap (push > capacity keeps newest N), count, at() ordering, blob round-trip, blob version/garbage rejection.

## Glue — `web_server_lite.cpp` (device)

- Static `LiteEventLog s_evlog;` + `s_evLogLastState` (int, init -999) + `s_evLogWasCharging`.
- **Hook in `web_server_lite_loop()`** (no MicroTasks listener — same loop-edge pattern as divert/limit): compute `evState = openevse_state_code(getDeviceState(), getState()==Disabled)`. On `evState != s_evLogLastState`:
  - if `s_clock && s_clock->valid()` (skip pre-NTP events, like upstream's 2021 guard), build an entry from the live getters: `epoch=nowUtc`, `type` = Warning when `evState` in 4..11 else Information, `managerState=getState()`, `evseState=evState`, `pilotA=getChargeCurrent`, `energyWh=getSessionWattHours`, `elapsedS=getSessionElapsed`, `tempC = isTemperatureValid()? getTemperature() : INT16_MIN`, `divertMode = s_divertCfg.enabled?2:1`, `shaper = s_shaperCfg.enabled?1:0`, `rfid = g_lite_rfid_status.last_uid`, `soc = s_feed.veh_soc_valid? s_feed.veh_soc : -1`; `s_evlog.push(e)`.
  - update `s_evLogLastState`.
- **Persist on session-complete edge** (charging→idle, reuse the charge-edge already tracked): `lite_config_save_eventlog(blob)`.
- **Load at `begin()`**: `lite_config_load_eventlog` → `s_evlog.fromBlob`.
- **`/logs` handler**: GET → `{"min":0,"max":0}`. **`/logs/{index}` handler** (`UriBraces`): GET, index 0 → serialize entries (snprintf per entry appended to a `String`, ISO-8601 via `lite_clock_iso8601`, `type`/`managerState` as strings, `rfidTag`/`soc` omitted when absent); other index → `[]`.
- Routes: `s_server.on("/logs", handle_logs); s_server.on(UriBraces("/logs/{}"), HTTP_GET, handle_logs_block);`

## Config store — `lite_config_store.{h,cpp}`

`bool lite_config_load_eventlog(void *buf, size_t cap, size_t &out_len);` / `bool lite_config_save_eventlog(const void *buf, size_t len);` over a `"evtlog"` KV blob (≤ ~2 KB).

## Testing & footprint

Native: `test/test_lite_event_log/` (ring + blob). Device build green. ~3–4 KB flash, ~2 KB RAM, KVDB writes only on session-complete. HW round-trip (state changes appear in `/logs/0`; survive a reboot after a session) folds into task #117.

## Deliberate scope

`/logs/export` deferred. `evseFlags` always 0 (lite has no flag word). One RAM block (no rotation/pagination). `temperatureMax == temperature` (lite has a single temp sensor).
