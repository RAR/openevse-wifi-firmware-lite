# Slice 1 — Config foundation + configurable charge limit + service-max (design)

**Status:** design for review — **persistence finalized (2026-06-13): FlashDB KVDB on the `kvs`
partition.** The LibreTiny EFM32 fork has no LittleFS and only an unimplemented `IPreferences`
interface, so config is stored via FlashDB's KV API directly. FlashDB + FAL + the `kvs` partition
are already wired into the framework build and **bench-verified persistent on the JuiceBox**
(a boot counter survived three power cycles). See "Persistence layer" below.
**Worktree:** `/home/rar/oevse/openevse-juicebox-lite` (branch `feature/juicebox-lite`)
**Context:** first slice of the JuiceBox-lite reuse roadmap. See the lift audit
`docs/superpowers/notes/2026-06-13-standard-fw-lift-audit.md`. This slice builds the **config
foundation** every later slice depends on, plus the **first method of the control-seam write
surface** (`setChargeCurrent`), and ships the user-facing **configurable charge limit + service
max**.

## Goal
Let the JuiceBox-lite charge-current limit be set at runtime over HTTP, clamped to a configurable
service-max ceiling, persisted to the FlashDB `kvs` partition, and restored on boot — replacing the
hard-coded `_chargeLimit = 6` the keepalive advertises today.

## Persistence layer (FlashDB KVDB on `kvs`)

The fork (`feature/silabs-efm32gg11-ota`, HEAD `a526fff`, already pulled in `/home/rar/dev/libretiny`)
provides everything; the firmware only consumes it:

- **Flash map** (`boards/_base/silabs-efm32gg11.json`; LibreTiny auto-generates the `FLASH_<NAME>_*`
  macros and the FAL partition table): boot `0x000000+0x008000`, ota1/bank A `0x008000+0x0F0000`,
  ota2/bank B `0x100000+0x0F0000`, **`kvs` `0x1F0000+0x008000`** (32 KiB, survives OTA),
  meta `0x1F8000+0x008000`. Both banks shrank 992→960 KiB to make room (app is ~472/960 KiB now).
- **FlashDB is fully vendored in the framework** — it builds `libflashdb.a` automatically; the
  firmware just `#include <flashdb.h>`. No sources to vendor, no `fdb_cfg.h` to add.
- **`FDB_WRITE_GRAN=32`** is set in the silabs builder (EFM32 flash is word-only; the default 8
  corrupted KV status markers and reformatted every boot). Do not override it.
- **API** (exact pattern, from the working `examples/KvsTest/src/main.cpp`):
```cpp
#include <flashdb.h>
static struct fdb_kvdb s_kvdb;
fdb_kvdb_init(&s_kvdb, "env", "kvs", NULL, NULL);          // db name "env", FAL partition "kvs", lock NULL
struct fdb_blob blob;
fdb_kv_get_blob(&s_kvdb, "key", fdb_blob_make(&blob, &buf, sizeof(buf)));  // blob.saved.len==0 ⇒ absent
fdb_kv_set_blob(&s_kvdb, "key", fdb_blob_make(&blob, &buf, sizeof(buf)));
```
- The lite build is a single-threaded `loop()` (web poll + backend run sequentially), so the `NULL`
  lock is safe — no concurrent KVDB access.

## Scope

**In:**
1. Initialize a FlashDB KVDB over the `kvs` partition at boot (re-include the currently-excluded
   `lite_config_store.cpp`; rewrite it from the LittleFS/JSON stub to the FlashDB KV API).
2. Generalize `lite_config_store` from WiFi-only to typed EVSE config, **mirroring upstream
   `app_config` key names** (`max_current_soft` / `max_current_hard`) so later module lifts find what
   they expect. Each value is its own KV blob.
3. Add the first control-seam write method: `setChargeCurrent(int)` / `getChargeCurrent()` on
   `LiteEvseBackend`, implemented by `JuiceBoxBackend` (drives the existing keepalive `_chargeLimit`).
4. HTTP `GET`/`POST /config` for the charge limit + service max, with clamp + persist + apply.
5. Boot flow: load config → clamp → apply to backend (folded into `web_server_lite_begin`).

**Out (explicitly deferred):**
- The **full claim/priority arbitration model** (multiple control clients) → **Slice 1.5**. This
  slice adds only the single-setpoint `setChargeCurrent` seam, no claim table.
- MQTT, NTP/scheduler, divert/shaper, RGB, web UI assets → later slices.
- **WiFi-cred migration to KVDB:** out of scope. `main_lite` keeps build-flag WiFi creds. The store's
  WiFi accessors are rewritten over KVDB for consistency but remain unused by `main_lite` this slice.
- Pre-seeding non–slice-1 config keys: unnecessary. KVDB is name-keyed, so later slices add their own
  keys with defaults — no migration. We only define the keys this slice uses.

## Architecture (Approach B — separate config layer, thin backend setter, web orchestrates)

```
HTTP POST /config ──▶ web_server_lite ──┐
                                        │ clamp(hard,[6,JB_ABS_MAX]); clamp(soft,[6,hard])
                                        ▼
                         lite_config_store  ──(fdb_kv_set_blob)──▶ FlashDB KVDB (kvs partition)
                                        │
                                        ▼
                         LiteEvseBackend.setChargeCurrent(soft)  ──▶ keepalive advertises $SL00N:soft
boot: lite_config_begin → web_server_lite_begin (load_evse → clamp → setChargeCurrent → cache)
```

Three responsibilities, cleanly separated:
- **`lite_config_store`** — persistence only (FlashDB KV blobs ⇄ struct). No clamp policy, no device I/O.
- **`LiteEvseBackend` / `JuiceBoxBackend`** — device control only. `setChargeCurrent(amps)` sets the
  RAM `_chargeLimit` the keepalive already advertises. No persistence, no HTTP.
- **`web_server_lite`** — orchestration + validation. Owns the clamp policy; reads/writes the store;
  applies to the backend; caches the active config in RAM (so `/status` and `GET /config` don't hit
  flash each poll). The boot path (its `begin`) does the same load→clamp→apply.

This is the only structure that stays clean when Slice 3 (MQTT) and Slice 4 (scheduler) *also* need
to read config and set the current — they go through the same store + `setChargeCurrent` seam.

## Components & changes

### 1. KVDB init + persistence backend swap
- **Files:** `platformio.ini` `[env:openevse_lite]` (`build_src_filter`), `src/lite/main_lite.cpp`,
  `src/lite/lite_config_store.{h,cpp}`.
- Re-add `+<lite/lite_config_store.cpp>` to `build_src_filter` (currently excluded).
- Rewrite `lite_config_store.cpp`: drop `#include <LittleFS.h>` and the JSON file I/O; add
  `#include <flashdb.h>`; `lite_config_begin()` becomes `fdb_kvdb_init(&s_kvdb, "env", "kvs", NULL, NULL)`
  (returns true on `FDB_NO_ERR`). The `-I src/lite` LittleFS shim include path is now unused but left
  in place (harmless; no other lite file includes `<LittleFS.h>` — verify during implementation).
- `main_lite.cpp`: call `lite_config_begin()` once in `setup()` after `s_backend.begin()` and before
  `web_server_lite_begin(s_backend)`.
- **No HW spike needed:** persistence is already bench-verified on this partition (boot counter
  across 3 power cycles). The old LittleFS-region risk is retired.

### 2. Config store: WiFi-only → typed EVSE config (FlashDB KV)
- **Files:** `src/lite/lite_config_store.{h,cpp}`.
- New struct + accessors (WiFi creds API kept, now KV-backed but unused by main this slice):
```cpp
struct LiteEvseConfig {
  int max_current_soft; // active charge-current setpoint (A) the keepalive advertises
  int max_current_hard; // service-max ceiling (A) — install rating; soft is clamped to this
};
bool lite_config_load_evse(LiteEvseConfig &out); // false if max_current_hard key absent → caller uses defaults
bool lite_config_save_evse(const LiteEvseConfig &in);
```
- Each field is its own KV blob under the upstream `app_config` key name (`max_current_soft`,
  `max_current_hard`), stored as a 4-byte `int`. `lite_config_load_evse` returns false when the
  `max_current_hard` key is absent (`blob.saved.len == 0`) → caller applies defaults; a present-but-
  partial store fills missing fields from defaults. Same `fdb_kvdb` handle as `lite_config_begin`.

### 3. Control-seam write method
- **Files:** `src/lite/lite_evse_backend.h`, `src/lite/juicebox_backend.{h,cpp}`.
- Add to the interface:
```cpp
virtual void setChargeCurrent(int amps) = 0; // desired charge current (A); backend may clamp to its own floor
virtual int  getChargeCurrent() const = 0;   // current advertised setpoint
```
- `JuiceBoxBackend::setChargeCurrent(amps)` sets `_chargeLimit = amps` (the keepalive already sends
  `juicebox_build_amps_set(_chargeLimit, …)`, which the Atmel further clamps to its 6 A floor and
  <81 ceiling). `getChargeCurrent()` returns `_chargeLimit`. **Keep the `_chargeLimit = 6` member
  initializer** as a safety default (so the advertised current is 6 A even if the boot apply is ever
  skipped); the boot path overwrites it with the loaded/clamped value.
- Note `getChargeCurrent()` (our setpoint) is distinct from the existing `getAmps()` (the Atmel's
  `$ES` `A` field = reported max/rating) — they are different values, both retained.
- `JuiceBoxBackend` is the only `LiteEvseBackend` implementation in the build (`lite_evse_stubs.cpp`
  is excluded), so adding pure virtuals breaks nothing else.

### 4. Clamp policy (pure, host-tested)
- **Files:** `src/lite/lite_charge_policy.{h,cpp}` (new, pure), test `test/test_lite_charge_policy/`,
  `platformio.ini` `[env:native]` (`build_src_filter`).
```cpp
static constexpr int JB_MIN_CURRENT = 6;   // J1772 floor
static constexpr int JB_ABS_MAX     = 48;  // absolute safety cap regardless of config
int lite_clamp_service_max(int hard);                 // clamp to [JB_MIN_CURRENT, JB_ABS_MAX]
int lite_clamp_charge_current(int soft, int hard);    // clamp to [JB_MIN_CURRENT, clamp_service_max(hard)]
```
- Pure integer functions, no Arduino deps → unit-tested in the native doctest env like
  `juicebox_proto`. Add `+<lite/lite_charge_policy.cpp>` to the native env `build_src_filter`.

### 5. Web endpoint (GET-set + connection reaper — revised after HW validation)
- **Files:** `src/lite/web_server_lite.cpp`.
- **`/config` is method-agnostic:** query params present (on **GET or POST**) ⇒ SET; absent ⇒ read
  current. `GET /config?max_current_soft=N&max_current_hard=M` (either or both) is the blessed path:
  no request body is ever required, so `curl 'http://ip/config?max_current_soft=20'`, evcc's
  generic-charger GET URLs, and the HA app all work. Handler: start from the cached config, overwrite
  present params (`mg_get_http_var(&hm->query_string, …)`), apply `lite_clamp_service_max` then
  `lite_clamp_charge_current`, persist via `lite_config_save_evse`, apply via
  `backend.setChargeCurrent`, update the RAM cache, respond with the **clamped** result as JSON (503
  if persistence failed but the value was applied). No-params ⇒ returns current (200).
- **Why GET-set (not POST-only):** Mongoose 6.18 will not fire `MG_EV_HTTP_REQUEST` for a POST that
  carries no `Content-Length` (e.g. a bodyless `curl -X POST`) — it waits for a body that never
  arrives, wedging the connection. A flood of these starves the connection pool (the server stops
  answering even GETs). GET has no body expectation, so it always dispatches. Real POST clients that
  send `Content-Length` (browsers/`fetch`, HA `rest_command`) still work; a bodyless POST is the only
  unsupported corner (it applies ~10s late with no response when the reaper closes it — see below —
  which is safe/clamped but undocumented usage; use GET).
- **Connection reaper:** `web_server_lite_loop()` closes any non-listening connection idle >10 s
  (`mg_time() - c->last_io_time`), so an incomplete request can never exhaust the pool. This is the
  robustness half of the fix.
- Add `max_current_soft` / `max_current_hard` to `/status` (from the RAM cache) for visibility.

### 6. Boot flow (in `web_server_lite_begin`)
- **Files:** `src/lite/web_server_lite.cpp`, `src/lite/main_lite.cpp`.
- `web_server_lite_begin(backend)` owns the RAM config cache (`static LiteEvseConfig s_cfg`). On
  begin: `lite_config_load_evse(s_cfg)`; if false, `s_cfg = { max_current_soft: 32, max_current_hard:
  32 }` (default — see Defaults); clamp hard then soft; `backend.setChargeCurrent(s_cfg.max_current_soft)`.
  `main_lite.cpp` only ensures `lite_config_begin()` ran first.

## Defaults & safety
- **Default `max_current_hard` = 32 A** and **`max_current_soft` = 32 A**. 32 A is the smallest
  JuiceBox model ever sold, so it's the safe out-of-box assumption for the hardware's rated current:
  a factory/unconfigured unit charges at its rated 32 A; the installer lowers `max_current_hard` if
  the circuit is smaller, or lowers `max_current_soft` to cap a session.
- `JB_ABS_MAX = 48 A` is a hard ceiling enforced even if config somehow holds a larger value.
- The Atmel independently clamps `<6 → 6` and rejects `≥81`; our clamp is the host-side policy layer.
- KVDB init failure → in-RAM defaults (32/32) + the unit still runs read + safe keepalive; the
  failure is logged (debug build) and `/config` POST returns 503 (can't persist).

## Data flow (set request)
`POST /config?max_current_soft=24` → start from cache → set soft=24 →
`clamp_service_max(hard=cached)` → `clamp_charge_current(24, hard)` → e.g. hard=16 ⇒ soft=16 →
`fdb_kv_set_blob` both keys → `backend.setChargeCurrent(16)` → update cache → next keepalive sends
`$SL002:16` → response `{"max_current_soft":16,"max_current_hard":16}`.

## Error handling
- Missing/unparseable query param → ignore that field (partial update allowed); if neither present →
  return the current config (200), i.e. a plain read.
- Out-of-range values → clamped, not rejected (response shows the clamped value).
- `max_current_hard` key absent on load → defaults, not an error.
- KVDB save failure → 503 + keep the in-RAM applied value (best effort).

## Testing
- **Native doctest** (`test/test_lite_charge_policy/`): clamp edge cases (below 6, above hard, above
  JB_ABS_MAX, hard<6, hard>JB_ABS_MAX, soft within range). Mirrors `test/test_juicebox_proto`'s
  runner. (The config store itself is a thin FlashDB wrapper with no host-testable logic — FlashDB
  isn't available in the native env — so it is validated on hardware, not in doctest.)
- **On-device HW:**
  1. `curl -X POST '…/config?max_current_hard=24&max_current_soft=20'` → `/status` shows 20/24 →
     VCOM keepalive shows `$SL002:20`.
  2. Reboot → `/status` still 20/24, keepalive still `$SL002:20` (persistence — KVDB survives reboot).
  3. `curl -X POST '…/config?max_current_soft=40'` with hard=24 → response + status clamp to 24.
- **Flash/RAM measurement** after the slice (roadmap requires it; bank is now 960 KiB).
- **Re-flash note:** the banks moved (992→960 KiB), so a full re-flash is required:
  `bootloader.bin@0x0`, `firmware_a.bin@0x8000`, `flash erase_address 0x1F0000 0xA000` (clears
  kvs+meta for a clean first boot). `scripts/lite_flash.sh` covers the two-image flash.

## Risks
- **KVDB persistence on EFM32:** retired — already bench-verified (boot counter across 3 reboots),
  with `FDB_WRITE_GRAN=32` confirmed as the fix for word-only flash.
- Mongoose 5.x `POST` query-param parsing — `mg_get_http_var(&hm->query_string, …)` works on the
  query string for any method (already used in upstream `web_server_rfid.cpp`); confirmed pattern.
- Default-32 service max is a permissive out-of-box ceiling — intentional (smallest unit sold);
  installer narrows it. Flagged for review.

## Follow-on
**Slice 1.5** grows `setChargeCurrent` into the full claim/setpoint surface
(`enable/disable/claim/release`, min/max/hardware-current, session counters, events) that unlocks
lifting limit/temp_throttle/divert/shaper/scheduler/mqtt per the audit.
