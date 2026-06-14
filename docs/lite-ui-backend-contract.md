# JuiceBox-lite Web UI — Backend Contract Brief

**For:** the UI agent building the trimmed lite settings UI (Slice 5).
**Target firmware:** branch `feature/juicebox-lite` (WGM160P / EFM32GG11 + WF200, via LibreTiny). NOT an ESP32 — no SPIFFS, no `data/` partition.
**As-built reference:** `src/lite/web_server_lite.cpp` (the only HTTP surface). Verified against commit `69dde2d`.

---

## 0. Read this first — two hard realities

1. **There is NO static-file serving today.** The lite web server is a pure JSON API on Mongoose. `GET /` currently returns the literal plain-text string `openevse-lite`; any unmatched path returns `404 not found`. So shipping a UI is *two* jobs:
   - (a) build a tiny frontend bundle, **and**
   - (b) add the firmware-side serving path for it (see §5). Do not assume an OpenEVSE-style `src/web_static/*.h` pipeline exists here — it does **not**. You're adding it.

2. **Flash budget is the gating constraint.** Lite is **dual-OTA**: the real app ceiling is the **960 KB OTA slot (983040 B)**, *not* 2 MB. PlatformIO's percentage reports against ~2 MB and is **roughly half the true figure** — ignore it; compute against 983040.
   - Current firmware: **519172 B used → ~52.8 % of the 960 KB slot → ~464 KB headroom.**
   - That headroom must cover the **gzipped** bundle *plus* the C array overhead of embedding it. Treat **~150 KB gzipped** as a comfortable target and **~250 KB gzipped** as the absolute ceiling. **No heavy framework** (no React/Vue/Angular runtime bundle). Vanilla JS, Preact, Svelte-compiled, or hand-rolled is the expectation. The full `gui-nightshift` bundle will NOT fit — do not start from it.

---

## 1. HTTP surface (all of it)

Single Mongoose event handler, routed by exact URI match. Responses are `Content-Type: application/json` unless noted. Every response is sent with `MG_F_SEND_AND_CLOSE` (no keep-alive).

| Method(s) | Path | Purpose |
|---|---|---|
| GET | `/status` | Full device status JSON (poll @ ≤1 Hz; see §2). |
| POST | `/status` | **3c feed-in** — push solar/grid/voltage/shaper sensor values (see §6). |
| GET, POST | `/config` | Read or write config. **Params on EITHER method set values** (see §3). |
| GET, POST, DELETE, PATCH | `/override` | Manual override control (see §4a). |
| GET, POST, DELETE | `/schedule`, `/schedule/<id>` | Charge scheduler CRUD (see §4b). |
| WS | `/ws` | Server pushes the `/status` JSON on connect + ~1 Hz thereafter. **Push-only** — inbound frames ignored; all control stays on HTTP. |

**Quirk that simplifies the client:** `/config` and `/override` accept their writes as **query-string params on a GET** as well as POST. A no-body GET like `GET /config?divert_enabled=1` performs the set. Prefer this — a no-Content-Length POST can wedge Mongoose waiting for a body. (For `/override` and `/schedule`, JSON-body POST is also fully supported; see below.)

---

## 2. `GET /status` — fields the UI can render

Emitted when present (keys absent rather than null when a value isn't available — code defensively). Types as serialized:

**State / telemetry**
- `state` (int, OpenEVSE code) + `status` (string, e.g. `"active"`, `"disabled"`, fault text)
- `amp` (int, live mA-derived amps), `pilot` (advertised setpoint A), `power` (W)
- `voltage` (int; derived `power/amp` while charging else 240 — also overridable via §6)
- `tempt`, `temp2` (temperature)
- `max_current_soft`, `max_current_hard`, `min_current_hard`, `available_current` (A)
- `manual_override` (0/1), `mode` (`"fast"`), `claims` (active claim count)

**Session energy**
- `wattsec`, `watthour`, `session_energy` (Wh), `elapsed` (s)

**Lifetime totals** (present once totals load)
- `total_energy`, `total_day`, `total_week`, `total_month`, `total_year` (**kWh**, float), `total_switches` (int)

**Identity / system**
- `firmware`, `version` (both = lite fw version string, currently `lite-4ws`)
- `ipaddress`, `ssid`, `srssi` (RSSI), `free_heap`/`freeram`, `uptime` (s)
- `schedule_version` (int, bumps on any schedule mutation — poll this to know when to refresh the schedule view)
- `time` (ISO-8601 **UTC**; **omitted until first SNTP sync** — absence means "clock not yet synced")

**Backend extras** (from the JuiceBox protocol layer): `hw`, `fw`, `protocol`, `md`, `wc`, `wr` (raw `$WR` fault string — the fault detail for state 8), `line`, `state_str`.

**Echoed feed** (only the keys that have been pushed via §6): `solar`, `grid_ie`, `voltage`, `shaper_live_pwr`.

---

## 3. `/config` — settings the UI edits

**Read:** `GET /config` returns the JSON below. **Write:** pass any subset as query params (on GET or POST); the response echoes the now-current, **clamped** config so the UI should re-render from the response, not from what it sent. Status `503` means "applied to RAM but flash persist failed" (won't survive reboot) — surface as a soft warning, not a hard error.

**Keys returned by `GET /config`:**

| Key | Type | Notes / valid range |
|---|---|---|
| `max_current_soft` | int A | Charge setpoint. Clamped to `[6 .. max_current_hard]` (6 A J1772 floor). |
| `max_current_hard` | int A | Service max. Clamped to hardware limit. |
| `divert_enabled` | bool (0/1) | Solar divert master switch. |
| `divert_type` | int | **0 = Solar, 1 = Grid.** (enum `LiteDivertType{Solar=0,Grid=1}`) |
| `divert_PV_ratio` | float | e.g. `1.1`. Note the **capital `PV`** in the key. |
| `divert_attack_smoothing_time` | int s | EMA attack tau. |
| `divert_decay_smoothing_time` | int s | EMA decay tau. |
| `divert_min_charge_time` | int s | Min charge duration before divert may stop. |
| `current_shaper_enabled` | bool (0/1) | Load-shaping master switch. |
| `current_shaper_max_pwr` | int W | Whole-premises power budget. |
| `current_shaper_smoothing_time` | int s | |
| `current_shaper_data_maxinterval` | int s | Stale-feed timeout — shaper fail-safes if `shaper_live_pwr` is older than this. |
| `current_shaper_min_pause_time` | int s | Min pause before shaper resumes. |

**Write-only keys (NOT in the GET response — the UI must track them locally if it shows them):**
- `tz_offset_min` (int minutes; only shifts the local day-bucketing of totals — `time` is always UTC)
- `sntp_hostname` (string, ≤47 chars)

Example sets: `GET /config?max_current_soft=20`, `GET /config?divert_enabled=1&divert_type=0&divert_PV_ratio=1.1`, `GET /config?current_shaper_enabled=1&current_shaper_max_pwr=7200`.

---

## 4a. `/override` — manual control

- `GET /override` → current override as JSON, or `{}` if none active.
- `POST /override` with JSON body → claim an override. Recognized body keys:
  - `state`: `"active"` | `"disabled"` | `"clear"` (clear = release/None)
  - `charge_current` (int A), `max_current` (int A), `auto_release` (bool)
  - `energy_limit` (int **Wh**), `time_limit` (int **s**) — **lite extension**, enforced session-relative. Returns `201 {"msg":"Created"}`.
- `DELETE /override` → release. `200 {"msg":"Deleted"}`.
- `PATCH /override` → toggle active/disabled. `200 {"msg":"Updated"}`.
- **Convenience:** `GET /override?state=active|disabled|release|clear` does the same as the body POST for the simple state cases (no body needed). Good for one-tap UI buttons.

GET-active response shape: `{state, charge_current?, max_current?, auto_release, energy_limit?, time_limit?, expired}`. **`expired:true` is sticky** — once a limit stops the session, nothing auto-resumes; the UI should show "limit reached, cleared" and require an explicit new override to restart.

**Priority semantics to reflect in the UI copy:** manual override (priority 1000) beats schedule (100) beats divert (50). The load-shaper caps *everyone* including manual (it claims at Safety/5000). So a manual override sets the *target*, but an active shaper can still cap the delivered current below it.

## 4b. `/schedule` — charge scheduler

- `GET /schedule` → JSON **array** of events: `[{id, state, time, days}, ...]`
  - `state`: `"active"` | `"disabled"`
  - `time`: `"HH:MM:SS"` (24 h, seconds optional on input; always emitted with seconds)
  - `days`: array of lowercase day names — `"sunday"`..`"saturday"`
- `POST /schedule` with body `{state, time, days, id?}` → upsert. `id` optional (auto-assigned max+1 if omitted/0). Returns `201 {"id":N}`, or `507 {"msg":"Schedule full"}` (max 16 events), or `400` on bad body.
- `DELETE /schedule/<id>` → remove. `200 {"msg":"Deleted"}` or `404`.
- After any mutation, `/status`.`schedule_version` increments — use it to invalidate a cached schedule view. Scheduling only fires when the clock is valid (`time` present in `/status`); surface "schedule needs NTP sync" if `time` is absent.

---

## 5. Serving the UI from firmware (the part that doesn't exist yet)

You must add this. Recommended minimal approach, in keeping with the lite codebase's "pure unit + thin glue" style:

1. **Build** the UI to a single (or few) gzip-compressed asset(s). Keep it tiny (§0).
2. **Embed** the gzipped bytes as a C array in a generated header under `src/lite/` (e.g. `web_ui_lite.h` with `const uint8_t INDEX_HTML_GZ[] = {...}` + length). A small build step or checked-in generated file both work — match what's least surprising; there's no existing generator to reuse.
3. **Serve** it by adding a branch to the URI router in `web_server_lite.cpp` (the `mg_vcmp(&hm->uri, ...)` chain ~line 557). Replace the current `GET /` plain-text stub with the gzipped asset: send `Content-Type: text/html` + `Content-Encoding: gzip`, body = the embedded bytes. Add routes for any extra assets (`/app.js` etc.) the same way. Keep `MG_F_SEND_AND_CLOSE`.
4. **Re-check the budget** after embedding: `pio run -e openevse_lite` then compute `firmware.bin size / 983040`. Must stay well under 100 % with margin for OTA.

**Mongoose note:** this fork uses the classic `mg_http_*` / `http_message` API (Mongoose 6.x style, `MongooseLite` vendored), not Mongoose 7 `mg_http_serve_*`. Follow the existing handlers' idiom exactly (`mg_send_head` + `mg_printf`).

---

## 6. `POST /status` — sensor feed-in (context only; not UI-driven)

External producers (e.g. a Home Assistant automation) push readings the divert/shaper logic consumes. Body is JSON, all keys optional (omit-when-absent): `{"solar":<W>, "grid_ie":<W>, "voltage":<V>, "shaper_live_pwr":<W>}`. Returns `200 {"msg":"OK"}`; values echo back in `GET /status`. The UI generally **reads** these back (to show "live solar/grid feed") rather than writing them, but a diagnostics page could expose a manual push.

---

## 7. Suggested UI scope (MVP, fits the budget)

1. **Dashboard** — state/status, amps/power, session energy, lifetime totals, RSSI/uptime, live solar/grid feed echo.
2. **Charge** — `max_current_soft`/`hard`; manual override (active/disabled/clear + charge_current + optional energy/time limit) via `/override`.
3. **Divert** — the 6 `divert_*` keys (enabled, type=Solar/Grid, PV ratio, smoothing, min charge).
4. **Shaper** — the 5 `current_shaper_*` keys.
5. **Schedule** — list/add/delete events via `/schedule`; warn if `time` absent (no NTP).
6. **System** — `tz_offset_min`, `sntp_hostname`, firmware/IP/SSID readout.

Live updates: subscribe to `/ws` for the dashboard; fall back to polling `GET /status` if the socket drops. Everything writable is GET-param-settable, so the client can be entirely fetch-based with no JSON-body plumbing if that keeps the bundle smaller.
