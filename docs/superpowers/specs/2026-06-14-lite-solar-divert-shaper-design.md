# JuiceBox-lite: Solar-Divert + Load-Shaping + HTTP Push-In (3c) — Design

**Date:** 2026-06-14
**Branch:** `feature/juicebox-lite`
**Worktree:** `/home/rar/oevse/openevse-juicebox-lite`

## Context

Slice 3 made the lite firmware speak the OpenEVSE local HTTP API the firstof9/openevse
Home Assistant integration consumes: **3a** `GET /status`, **3b** `/override` control, **3d**
`/ws` push. This design adds the two remaining pieces:

- **3c — `POST /status` receiver:** let an external producer (the HA integration, scripts)
  push sensor data **into** the unit. The firstof9 integration *already* POSTs `solar`,
  `grid_ie`, `voltage`, `shaper_live_pwr` to `/status` — so "send data in from the HA side"
  needs **zero HA-side changes**; lite just has to receive what it already sends.
- **Solar-divert + load-shaping:** lift the standard firmware's `divert.cpp` and
  `current_shaper.cpp` onto the lite control seam, consuming the pushed feed. Lite has **no
  MQTT client**, so the 3c receiver *is* the feed source (upstream reads these from MQTT).

The Slice 1.5 control seam (`LiteEvseManager` + `lite_evse_arbitrate`, per-property
highest-priority-wins, `EvseProperties` = state/charge_current/max_current/auto_release) is
exactly what divert/shaper plug into. Override and the scheduler already claim through it.

## Goal

A JuiceBox-lite unit that, fed solar/grid/voltage/site-power over `POST /status`, autonomously
modulates charge current to follow excess solar (divert) and caps total current to a site-power
budget (shaper) — behaviourally identical to standard OpenEVSE, driving the ATmega via the
existing `$AL` keepalive — with config key names and semantics that the HA integration and
OpenEVSE dashboards already understand.

## Design decisions

- **D1 — Full OpenEVSE parity.** Transcribe the upstream divert/shaper math verbatim; mirror
  config key names + defaults so existing tooling drives it unchanged.
- **D2 — Autonomous, OpenEVSE priority order.** Use the upstream priority constants exactly
  (`Default 10 < Divert 50 < Timer 100 < Manual 1000 < Limit 1100 < Safety 5000`). Consequence
  (corrects an over-statement during brainstorm): **divert is *below* the schedule.** A schedule
  window charges at its full rate; divert solar-follows whenever the schedule is not actively
  claiming. Manual override always wins. Shaper caps everyone (incl. manual) via `Safety`.
- **D3 — Fail-safe on stale feed.** If the pushed feed goes stale, charging pauses (see Error
  Handling), never silently grid-charges at a stale setpoint.
- **D4 — Approach C (hybrid pure units).** Transcribe the algorithm into native-testable pure
  units (`lite_input_filter`, `lite_divert`, `lite_shaper`) + thin glue. HW validation is
  blocked on the GFI-faulted bench, so native tests are the confidence; pure cores are
  mandatory, mirroring `lite_override`/`lite_schedule`.
- **D5 — Divert "stop" = claim `Disabled@Default(10)`, not release** *(refines the brainstorm's
  "release", per reading the source)*. Upstream claims `Disabled` at the lowest priority when
  solar is insufficient: a schedule/manual claim still overrides it, but it explicitly disables
  when divert is the sole claimant. Safer than releasing, because lite's `$AL` keepalive would
  otherwise charge at the no-claim default. **Flagged for review.**

## Architecture

Three new pure units in `src/lite/` (no `OPENEVSE_LITE` guard; `.cpp` added to `[env:native]`
`build_src_filter`; doctest suites in `test/test_<unit>/`), plus glue.

### `lite_input_filter.{h,cpp}` — shared exponential smoothing (transcribes `InputFilter`)

```cpp
// Exponential decay toward `input`. tau in seconds; delta_s = seconds since last sample.
// factor = (tau>0) ? 1 - exp(-delta_s / max(tau, LITE_FILTER_MIN_TAU)) : 1.0   (MIN_TAU = 10)
// returns filtered + factor * (input - filtered).  Pure — no millis() inside.
double lite_input_filter(double input, double filtered, uint32_t tau_s, uint32_t delta_s);
```
Caller tracks the last-sample timestamp and passes `delta_s` (keeps the unit pure/testable).

### `lite_feed.{h,cpp}` — the 3c push-in store

Holds the last pushed `solar` (W, int), `grid_ie` (W, int, negative = export), `voltage` (V,
double), `shaper_live_pwr` (W, int), each with a `last_update_ms` and a validity flag. Pure
predicate `bool lite_feed_fresh(uint32_t last_update_ms, uint32_t now_ms, uint32_t max_age_ms)`
(unsigned-subtraction wrap-safe). Setters stamp the time; getters expose value + freshness. The
3c HTTP handler is the only writer; divert/shaper glue are readers.

### `lite_divert.{h,cpp}` — solar diversion (transcribes `DivertTask::update_state` Eco path)

Pure compute, no Arduino/seam deps. One call per loop tick:

```cpp
enum class LiteDivertType { Solar = 0, Grid = 1 };
enum class LiteDivertAction { Charge, Stop, Hold };   // Hold = within hysteresis, no change

struct LiteDivertCfg { LiteDivertType type; double pv_ratio;       // dpr 1.1
                       uint32_t attack_s, decay_s, min_charge_s;   // das 20, dds 600, dt 600
                       int min_current_a; };                       // 6 (J1772 floor)
struct LiteDivertState { double smoothed_available; };             // carried across calls

struct LiteDivertResult { LiteDivertAction action; int charge_rate_a; double available, smoothed; };

LiteDivertResult lite_divert_eval(const LiteDivertCfg&, LiteDivertState&,
    int solar_w, int grid_ie_w, double voltage,
    int evse_present_a,           // EVSE's own contribution (see note)
    bool currently_active, bool min_charge_elapsed, uint32_t delta_s);
```

Verbatim algorithm (Eco):
- **Available current**
  - `GRID`: `Igrid = grid_ie/voltage - evse_present_a`; if `Igrid < 0` (export):
    `reserve = 1000*((pv_ratio>1)?(pv_ratio-1):0)/voltage`; `available = -Igrid - reserve`;
    else `available = 0`.
  - `SOLAR`: `available = solar/voltage`.
  - clamp `available >= 0`.
- **Smoothing**: `tau = (available > smoothed) ? attack_s : decay_s`;
  `smoothed = lite_input_filter(available, smoothed, tau, delta_s)`.
- **Charge rate**: `rate = floor(available)`; if `(available - rate) > min(1.0, pv_ratio)` → `rate += 1`.
- **Trigger / hysteresis** (`HYST = 0.5 A`): `trigger = min_current_a * min(1.0, pv_ratio)`.
  - `smoothed >= trigger + HYST` → `Charge` at `rate`.
  - `smoothed <= trigger` → `Stop` **only if** `currently_active && min_charge_elapsed`; else `Hold`.
  - in between → `Hold`.
- `min_charge_elapsed` reflects the min-charge-time hysteresis (set a timer when charging
  begins; the glue owns the timer using `millis()`, a relative duration — no wall clock needed).

**Lite adaptation (note):** upstream subtracts the measured `getAmps()` in GRID mode. JuiceBox
`$ES` does not expose reliable live draw (its `A` is the active *limit*), so lite passes the
**last commanded charge current** as `evse_present_a` (a faithful proxy: the car draws ~what we
commanded while charging). Unused in SOLAR mode.

### `lite_shaper.{h,cpp}` — site-power cap (transcribes `CurrentShaperTask::shapeCurrent`)

```cpp
struct LiteShaperCfg { uint32_t max_pwr_w;            // smp 0 (0 = no budget)
                       uint32_t smoothing_s; };       // sst 60
struct LiteShaperState { double smoothed_live_pwr; bool paused; };

// returns the max-current cap (A). Caller treats cap < min_current as "pause".
double lite_shaper_cap(const LiteShaperCfg&, LiteShaperState&,
    int live_pwr_w, double voltage, int evse_present_a,
    int solar_w, bool divert_solar_enabled, uint32_t delta_s);
```
Verbatim: when not paused `smoothed = live`; when recovering, rising power is taken immediately
else `lite_input_filter(live, smoothed, smoothing_s, delta_s)`. If divert is enabled in SOLAR
mode, `max_pwr += solar` (self-production added to budget — upstream behavior). Single-phase:
`cap = (max_pwr - livepwr)/voltage + evse_present_a`. (Three-phase path is out of scope; J1772 /
JuiceBox is single-phase — `threephase` left unimplemented/false.)

### Glue

- **`web_server_lite.cpp` — `POST /status`:** new method branch (mirrors the existing `/override`
  and `/schedule` JSON-body parse). Parse `solar`/`grid_ie`/`voltage`/`shaper_live_pwr` (each
  optional; `omit-when-absent`), write into `lite_feed` with timestamps, respond `200`. GET
  `/status` already exists (3a) and now also echoes the freshest pushed values it stores.
- **`main_lite.cpp` loop:** each tick, with config + feed in hand:
  1. `cap = lite_shaper_cap(...)`; if shaper enabled → `s_manager.claim(Shaper, Priority_Safety,
     {MaxCurrent=floor(cap)})`, or claim `Disabled@Limit(1100)` when paused/stale; release when
     shaper disabled.
  2. `r = lite_divert_eval(...)`; `Charge` → `claim(Divert, Priority_Divert(50),
     {Active, ChargeCurrent=rate})`; `Stop` → `claim(Divert, Priority_Default(10), {Disabled})`;
     `Hold` → leave the existing claim; Normal mode (divert disabled) → `release(Divert)`.
  3. `lite_evse_arbitrate` resolves state + charge_current + max_current (highest-priority per
     property); `LiteEvseManager::apply()` already clamps `charge_current` to `max_current` then
     to hardware via `lite_clamp_charge_current` (6 A floor / hw cap) → `$AL`. A resolved current
     below the 6 A floor stops charging (J1772 can't trickle below 6 A).

### Config (FlashDB store + `/config`, parity key names & defaults)

| key | short | default | meaning |
|---|---|---|---|
| `divert_enabled` | de | false | divert on/off (Eco when on, else Normal=release) |
| `divert_type` | dm | 0 | 0=SOLAR, 1=GRID |
| `divert_PV_ratio` | dpr | 1.1 | marginal PV fraction / reserve |
| `divert_attack_smoothing_time` | das | 20 | smoothing tau when power rising (s) |
| `divert_decay_smoothing_time` | dds | 600 | smoothing tau when power falling (s) |
| `divert_min_charge_time` | dt | 600 | min charge time before divert may stop (s) |
| `current_shaper_enabled` | se | false | shaper on/off |
| `current_shaper_max_pwr` | smp | 0 | site power budget (W) |
| `current_shaper_smoothing_time` | sst | 60 | shaper smoothing tau (s) |
| `current_shaper_data_maxinterval` | sdm | 120 | shaper feed staleness timeout (s) |
| `current_shaper_min_pause_time` | spt | 300 | min pause before shaper resumes (s) |

(Lite config keys mirror upstream names so the same client/docs apply; stored in the existing
lite FlashDB config blob, exposed via the existing `/config` GET/POST.)

## Data flow

```
HA integration ─POST /status {solar,grid_ie,voltage,shaper_live_pwr}→ lite_feed(+timestamps)
main loop tick:
  shaper: lite_shaper_cap(feed,cfg) → cap → claim(Shaper, Safety, MaxCurrent) | Disabled(Limit) if pause/stale
  divert: lite_divert_eval(feed,voltage,state,cfg) → Charge|Stop|Hold
            → claim(Divert,50,Active@rate) | claim(Default,10,Disabled) | leave
  arbitrate (per-property hi-prio): manual(1000) > [Shaper cap @5000 always] ; schedule(100) > divert(50)
  apply(): clamp charge_current to max_current, to hardware (6A floor) → backend $AL keepalive
GET /status / /ws unchanged: also echo freshest pushed feed values.
```

## Error handling / edge cases

- **Divert feed stale** (`solar`/`grid_ie` not refreshed within `current_shaper_data_maxinterval`
  — reuse the same staleness window): divert evaluates with no fresh data → treats available as
  0 → `Stop` after min-charge-time → `Disabled@Default`. Fail-safe (D3).
- **Shaper feed stale** (`shaper_live_pwr` not refreshed within `current_shaper_data_maxinterval`):
  pause — claim `Disabled@Limit(1100)` (upstream behavior). Resume only after
  `current_shaper_min_pause_time` and cap recovers above `min_current + HYST`.
- **Cap below 6 A / target below 6 A**: charging stops (J1772 floor); shaper pause hysteresis
  prevents flapping.
- **min-charge-time**: once divert starts charging it will not stop before `divert_min_charge_time`
  even if solar drops (relay/car wear protection), exactly as upstream.
- **Missing voltage in feed**: use a configured nominal (default 240 V) so divert math is defined
  before the first `voltage` POST.
- **omit-when-absent**: a `POST /status` with only some keys updates only those; never treat a
  missing key as 0.

## Testing

Native doctest suites (the only real confidence — charge control can't run on the GFI-faulted
bench):
- `test_lite_input_filter`: factor math, tau<MIN_TAU clamp, tau=0 passthrough, decay toward input.
- `test_lite_feed`: freshness boundary + millis wrap.
- `test_lite_divert`: SOLAR available=solar/V; GRID export/import + reserve; charge-rate rounding
  vs pv_ratio; trigger/hysteresis Charge/Stop/Hold; min-charge-time gating of Stop; smoothing.
- `test_lite_shaper`: cap = (max_pwr−livepwr)/V + evse_a; solar added in SOLAR-divert; smoothing;
  pause when cap<min; resume after min_pause + hysteresis; stale → pause.

Glue (HTTP handler, loop claims) is thin and device-only; **on-device validation deferred** to a
non-GFI-faulted unit with a real load + a live solar/grid feed (same ceiling as override/schedule
charge control). Build gate: device `pio run -e openevse_lite` green (record flash %; expected
~+15–30 KB, well under the 960 KB OTA slot — currently 51.4%); full `pio test -e native` green.

## Sub-slices (for the implementation plan)

1. **3c feed receiver:** `lite_input_filter` + `lite_feed` (+ tests) + `POST /status` handler +
   `/status` echo of stored values. Independently shippable & HW-checkable over WiFi (no charge
   control), like 3a/3d.
2. **Divert:** `lite_divert` (+ tests) + config keys + loop glue claiming via the seam.
3. **Shaper:** `lite_shaper` (+ tests) + config keys + loop glue (MaxCurrent claim + pause), and
   the divert/shaper interaction (`max_pwr += solar`).

## Out of scope

- Web settings UI for these knobs — that's Slice 5 (deferred; config is settable via `/config`).
- Three-phase (J1772/JuiceBox is single-phase; `threephase` path not implemented).
- MQTT feed (lite has no MQTT client; the 3c POST path is the feed).
- Outbound publishing (Emoncms/MQTT export) — unrelated separate feature.
- On-device charge-control validation — deferred to a complete unit.

## Open items flagged for review

- **D5**: divert-stop as `Disabled@Default(10)` vs a plain release — confirm the upstream-faithful
  `Disabled@Default` is what you want (it is safer here).
- **Shaper caps manual override** (via `Safety` priority) — confirm site-power limit should bind
  even a manual override (upstream behavior; matches "shaper clamps everyone").
