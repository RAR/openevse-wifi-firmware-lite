# JuiceBox fault → UI contract (for the GUI)

The JuiceBox has a richer fault taxonomy than OpenEVSE's fixed state-code table, so
the firmware does two things in `/status`:

1. Sets the numeric **`state`** to the *closest* OpenEVSE code (so existing label
   logic / Home Assistant aren't actively wrong), and
2. Always carries the **exact** JuiceBox fault text in **`wr`**.

For faults that map cleanly the numeric `state` is correct on its own. For the few
with **no** OpenEVSE equivalent (pilot-generation, FW self-test) the numeric code is
only *nearest-fit* — the GUI should prefer `wr` for the displayed text on error states.

## Fields to read (all already in `/status`)

| field | meaning |
|---|---|
| `state` (int) | OpenEVSE state code. Errors are `4..11`. Fault-mapped (see table). |
| `state_str` (string) | `"error"` for any fault (generic). |
| `status` (string) | `"error"` for any fault (generic). |
| `wr` (string) | **Exact** JuiceBox fault, format `"NNN:Human text:"` — e.g. `"005:Pilot Signal Gen Fail:"`. Present only while a fault is latched. Sticky (last value). |
| `comms_online` (bool) | true once the ATmega is talking. If false, treat fault text as stale. |

`wr` parse: `/^(\d+):(.*?):?$/` → group 1 = JuiceBox code (note leading zeros), group 2 = text.

## Recommended GUI change

For `state` in `4..11` (error), if `wr` is present **and** `comms_online`, show the
JuiceBox text from `wr` as the fault detail instead of the generic OpenEVSE label.
Keep the existing `state`-based icon/colour (all errors are red-danger anyway).

Optionally add localized labels keyed on the JuiceBox code (more precise than the
parsed English text) — table below.

## JuiceBox `$WR` fault taxonomy → firmware mapping

| `$WR` | JuiceBox text | firmware `state` | OpenEVSE label (current UI) | exact? |
|---|---|---|---|---|
| 001 | FW Self Tests Failed | 9 | GFCI self-test failed | ~nearest |
| 003 | No GND | 7 | No ground | ✓ exact |
| 004 | Short Circuit Pilot | 5 | Diode check failed | ~nearest (pilot) |
| 005 | Pilot Signal Gen Fail | 5 | Diode check failed | ~nearest (pilot) |
| 006 | GFI Auto Test Fail | 9 | GFCI self-test failed | ✓ exact |
| 007 | Relay Stuck Closed | 8 | Stuck relay | ✓ exact |
| 008 | Ground Fault Int Lockout | 6 | GFCI fault | ✓ exact |
| 101 | Ground Fault Int | 6 | GFCI fault | ✓ exact |
| 102 | Relay Stuck Open | 8 | Stuck relay | ~nearest |

The four marked `~nearest` are why the GUI should show `wr` text: the OpenEVSE label
table (`getStateDesc`) has no entry for pilot-generation, short-circuit-pilot, FW
self-test, or relay-stuck-*open*. With `wr` displayed, all nine read exactly.

Suggested new i18n keys (keyed on the JuiceBox code) if you want localized strings
rather than the raw English from `wr`:
`jb-fault.001` … `jb-fault.008`, `jb-fault.101`, `jb-fault.102` with the texts above.

## Firmware source of truth
`src/lite/juicebox_proto.cpp` — `juicebox_wr_code()` + `juicebox_fault_openevse_state()`
(both native-tested in `test/test_juicebox_proto/`). Applied in
`JuiceBoxBackend::addStatusFields()`.
