# Lite Modular EVSE Backend + JuiceBox `$`-Protocol — Design

**Date:** 2026-06-12
**Branch:** `feature/juicebox-lite`
**Status:** Approved (brainstorming), pending spec review → implementation plan

## Goal

Give the OpenEVSE *lite* firmware a pluggable EVSE-device backend, and ship its
first concrete implementation: the JuiceBox MCU's proprietary `$`-framed serial
protocol. Scope this slice to **read + heartbeat** — decode the MCU's status and
keep it online — with **no charge-current control**.

## Background & motivation

"Lite" is OpenEVSE-companion firmware for non-ESP, LibreTiny-class boards (first
target: the Enel X JuiceBox 40 / Silicon Labs WGM160P, where our firmware
replaces the stock WiFi module). The EVSE controller our firmware talks to over
the UART is the *variable* part: the JuiceBox is **not** an OpenEVSE controller
and does not speak RAPI.

Today the lite build instantiates the stock OpenEVSE stack —
`EvseManager` → `OpenEVSEClass` → `RapiSender` — and `/status` reads five
accessors off `EvseManager`. That stack drives an OpenEVSE controller via RAPI
command/response and can never get a valid reply from the JuiceBox MCU, so
`/status` reports defaults (`state:0`, `voltage:240`). Everything `EvseManager`
exists to coordinate (divert, current-shaper, limits) is already stubbed out in
lite (`lite_evse_stubs.cpp`), so `EvseManager` here is a non-functional RAPI
wrapper.

We replace it with a small, focused, host-testable backend behind a thin
interface, so lite can support different controllers per board.

## The JuiceBox `$`-protocol (from the stock ATmega flash dump)

Source of truth: `/home/rar/device-configs/esphome/juicebox/stock/`
(`juicebox_atmega_flash.bin`, 32 KB). The ATmega is the EVSE controller on the
far end of our UART; the `$`-strings are its comms with the (replaced) WiFi
module.

**Framing:** `$<2-char type><3-hex length>:<payload>` where `length` is the hex
count of payload characters. Verified: `$ES01C:S00,L00,T00,H00,A00,P000,F00` —
payload is 28 chars = `0x01C`.

**MCU → WiFi (we receive):**

| Type | Example | Meaning |
|------|---------|---------|
| `$ES` | `$ES01C:S00,L00,T00,H00,A00,P000,F00` | status: **S**tate, **L**ine, **T**emp, **H**?, **A**mps, **P**ower(3), **F**ault |
| `$HW` | `$HW011:001.0001701.A.002` | hardware revision |
| `$FW` | `$FW006:100102` | firmware version |
| `$PV` | `$PV002:20` | protocol version (20) |
| `$MD` | `$MD:No comm signal. Switching to offline mode` | human-readable diagnostics (GFI, DFU, compile date, offline) |
| `$BP`/`$CR`/`$DF`/`$IP`/`$LG`/`$PG`/`$VG` | — | button-press / current-rating / diode-fault / etc. (decoded opportunistically) |

**WiFi → MCU (we send):**

| Type | Notes |
|------|-------|
| `$WC___:` / `$WR___:` | the two commands the MCU expects; it errors `"WC received when not expecting it"` |
| amps-set | 2-digit payload (`length=002`), value must be `< 80` (`"_L amps value of XX received. Should be less than 80"`). **Built but not sent this slice.** |

**Offline behaviour:** the MCU logs `"No comm signal. Switching to offline mode"`
when the WiFi side goes quiet — so we must send a periodic heartbeat to keep it
online. Exact heartbeat bytes + interval are the one genuine unknown (see Task 1).

## Architecture (Approach A — standalone backend, compile-time selected)

`/status` and `main_lite` depend only on an abstract `LiteEvseBackend`. Concrete
backends sit behind it; exactly one is compiled per board via a build flag.

```
LiteEvseBackend (abstract)              ← web_server_lite + main_lite depend ONLY on this
   ├─ begin() / loop()                    own the UART, pump the protocol
   ├─ isOnline()                          liveness from lastRxMillis
   ├─ getState() → LiteEvseState          each backend maps native codes → canonical enum
   ├─ getAmps() / getPower() / getTemp() / getFault()   normalized surface
   ├─ addStatusFields(JsonDocument&)      backend-specific extras
   └─ [future] setCurrent(amps)           control surface; default no-op

JuiceBoxBackend : LiteEvseBackend        first impl: owns juicebox_proto + heartbeat
   (RapiBackend : LiteEvseBackend)         obvious future second: wraps EvseManager
```

### Modules

- **`src/lite/juicebox_proto.{h,cpp}`** — pure, zero Arduino deps, host-testable.
  - Frame layer: incremental parse of `$<type><3-hex len>:<payload>` from a byte
    stream (handles partial reads and garbage via resync); build a frame
    (type + payload → `$..NNN:..`).
  - Decode: `$ES` payload → field struct (state, line, temp, H, amps, power,
    fault); `$HW`/`$FW`/`$PV` → identity strings; `$MD` → last-diagnostic string.
  - Command build: heartbeat frame(s); amps-set builder (written, **uncalled**).
- **`src/lite/lite_evse_backend.h`** — abstract `LiteEvseBackend` interface +
  canonical `LiteEvseState` enum.
- **`src/lite/juicebox_backend.{h,cpp}`** — `LiteEvseBackend` impl: owns `Serial`,
  pumps `juicebox_proto` each `loop()`, holds latest state + `lastRxMillis`, maps
  JB `S` → `LiteEvseState`, sends the heartbeat on a timer. Gated
  `#if defined(OPENEVSE_LITE) && defined(LITE_EVSE_BACKEND_JUICEBOX)`.
- **`src/lite/web_server_lite.cpp`** — five `s_evse->getX()` calls become
  `s_backend->getX()`; takes `LiteEvseBackend&`.
- **`src/lite/main_lite.cpp`** — instantiate the one backend the build flag
  selects (`#error` if none); pass it to the web server.

### Backend selection

Compile-time per board. `[env:openevse_lite]` (WGM160P / JuiceBox 40) gets
`-D LITE_EVSE_BACKEND_JUICEBOX`. Each backend `.cpp` is wrapped in its own
`#if defined(OPENEVSE_LITE) && defined(LITE_EVSE_BACKEND_<X>)` guard, so
`build_src_filter` may list all backends and the unselected ones compile to
nothing (mirrors the existing `#ifdef OPENEVSE_LITE` convention). A new board =
new env + new flag + new `*_backend.cpp`, with **zero churn** in web/main.

## `/status` mapping

Backend-agnostic core fields, plus backend extras via `addStatusFields`:

| `/status` key | source | notes |
|---|---|---|
| `state` | `getState()` | canonical `LiteEvseState`; JB maps its `S` code (table RE'd in Task 1) |
| `amp` | `$ES` A | charge current, amps |
| `power` | `$ES` P | watts (3-digit field) |
| `temp` | `$ES` T | controller temp |
| `fault` | `$ES` F | fault/GFI code; `$MD` carries detail |
| `online` | `isOnline()` | from `lastRxMillis` freshness |
| `protocol` / `fw` / `hw` | `$PV` / `$FW` / `$HW` | identity, captured at startup |
| `free_heap` / `uptime` | unchanged | already present |

The hardcoded `voltage:240` is **dropped** — `$ES` has no voltage field. `$ES`
fields `L` and `H` are decoded into the struct but kept internal until Task 1 RE
confirms their meaning; surfaced later if useful.

## Data flow & heartbeat

```
MCU ──$ES…/$HW…/$MD…──▶ Serial(9600) ──▶ JuiceBoxBackend.loop()
                                              ├─ frame parser → LiteEvseState
                                              └─ heartbeat timer ──$WC…/$WR…──▶ Serial ──▶ MCU
web /status ──▶ reads LiteEvseState (no UART I/O on the request path)
```

Heartbeat is sent on a timer at an interval safely under the MCU's offline
timeout, and **only after valid MCU traffic has been observed** (don't transmit
into a silent/disconnected line). Exact bytes + cadence come from Task 1.

## Error handling, offline & safety

- **Resync:** parser discards bytes until `$`, validates the 3-hex length, and on
  a malformed or over-length frame drops it and re-hunts `$`. Never blocks.
- **Offline detection:** `now - lastRxMillis` over a threshold ⇒ `isOnline()`
  false; `/status` reports `online:false` rather than serving stale state as live.
- **Safety (read + heartbeat scope):** the only thing we transmit is the
  keep-alive. The amps-set command exists in `juicebox_proto` but has **no caller
  and no web route** this slice — there is no code path that changes charge
  current. Heartbeat transmit is guarded on having seen valid MCU traffic first.

## Testing

- **`test/test_juicebox_proto/` — native doctest env** (like `test_fake_evse` and
  the `ha_*` suites): feed synthetic/captured `$ES`/`$HW`/`$FW`/`$PV`/`$MD` byte
  sequences — including split-across-reads and garbage-prefixed — and assert the
  decoded field struct, identity strings, canonical-state mapping, frame-build
  round-trips, and the exact heartbeat frame bytes.
- **On-device:** flash; `curl /status` shows live `state`/`amp`/`power`/`temp`
  changing with the JB and `online:true`; the MCU does **not** log
  `"Switching to offline mode"` (proves the heartbeat holds).

## Build wiring

In `[env:openevse_lite]`:
- **Remove** from the build/lib_deps: `evse_man.cpp`, `evse_monitor.cpp`,
  `event_log.cpp`, `openevse.cpp`, `lite_evse_stubs.cpp` (divert/shaper no longer
  referenced), and the OpenEVSE + StreamSpy `lib_deps`.
- **Add:** `+<lite/juicebox_proto.cpp> +<lite/juicebox_backend.cpp>` and
  `-D LITE_EVSE_BACKEND_JUICEBOX`.
- `debug_lite.cpp` keeps bringing up `Serial @ 9600`, but the StreamSpy/RAPI
  framing rationale is replaced — the backend owns the line directly.

Net flash should shrink (dropping the OpenEVSE/RAPI client + StreamSpy).

## Out of scope (this slice)

- Charge-current control (amps-set), start/stop. Builder exists but is uncalled.
- A second backend (e.g. `RapiBackend`) — the interface is shaped to allow it,
  but YAGNI until a board needs it.
- Voltage in `/status` (no `$ES` field; revisit if `$VG`/derivation yields it).
- Persisted config / web control surface for the backend.

## Key risks

- **Task 1 heartbeat RE is the critical path.** If the keep-alive turns out to
  embed a current setpoint, "heartbeat without changing current" means echoing
  the MCU's reported/own current as a no-op — to be confirmed from the
  disassembly + a live capture before we transmit.
- **Field semantics** of `$ES` `L`/`H` (and the JB state codes) require RE; the
  parser stores them regardless, so decode work doesn't block framing.
