# JuiceBox-Lite MQTT Telemetry — Design Spec

**Date:** 2026-06-15
**Status:** Approved-by-delegation (user delegated remaining decisions: "Work it out on your own.")
**Slice:** MQTT telemetry publisher (runs *in addition to* the OpenEVSE local-API / firstof9 HA integration — Slice 3).

> User review gate was waived: the user explicitly delegated the remaining design
> decisions and went offline. This spec records the decisions made on their behalf.
> HW round-trip validation is deferred to the user's bench.

## Goal

Add an outbound-only MQTT **telemetry** publisher to the JuiceBox-lite firmware that
mirrors the standard OpenEVSE MQTT topic convention (`<base>/<key>` per-field), so the
existing Home Assistant OpenEVSE-MQTT integration and Node-RED flows work unchanged.
Plaintext (no TLS). No control path — telemetry only.

## Scope

**In scope**
- Plaintext MQTT client over LibreTiny `WiFiClient` (`knolleary/PubSubClient`).
- Per-field publishes `<base>/<key>` from the existing status document.
- Cadence: every 30 s idle, every 5 s while charging (idle period configurable).
- Retained availability topic `<base>/available` = `online` / `offline` via LWT.
- Retained `<base>/announce` JSON (firmware, ip, http url, id) on (re)connect.
- Config: `LiteMqttConfig` FlashDB blob; read/write via existing `/config` GET+POST.
- Non-blocking reconnect with backoff in the main loop.
- Native doctest coverage of the pure decision logic.

**Out of scope (YAGNI for v1)**
- TLS / MQTTS (`WiFiClientSecure`). Plaintext only — LAN-trust.
- Any `/set` / control subscription (override, claim, limit, config, restart, rapi).
- HA MQTT auto-discovery payloads (the OpenEVSE HA-MQTT integration already maps
  per-field topics; discovery is a possible future slice).
- A bespoke web settings page (the GUI agent owns `openevse-gui-lite`; config is
  reachable today via `/config`).

## Architecture

Three units, matching the lite "pure core + device glue" pattern (pure logic is
host-tested; device I/O is `#ifdef OPENEVSE_LITE`):

### 1. `lite_mqtt_policy.{h,cpp}` — pure, native-tested
No Arduino/PubSubClient/WiFi includes, so it compiles and tests on the `native` env.

```cpp
#pragma once
#include <stdint.h>
#include <string>

// Publish cadence: true when now is at/after the next scheduled publish.
// While charging, use chargeMs; otherwise idleMs. lastMs == 0 means "never published"
// -> always due. Uses (now - last) >= period with unsigned wrap safety.
bool mqtt_should_publish(uint32_t lastMs, uint32_t nowMs,
                         uint32_t idleMs, uint32_t chargeMs, bool charging);

// Reconnect backoff: true when (now - lastAttempt) >= backoffMs (or lastAttempt == 0).
bool mqtt_backoff_due(uint32_t lastAttemptMs, uint32_t nowMs, uint32_t backoffMs);

// "<base>/<key>" with exactly one slash: trailing slashes on base and leading
// slashes on key are trimmed. Empty base -> just key; empty key -> just base.
std::string mqtt_topic_join(const std::string &base, const std::string &key);

// Default base topic when config topic is blank: "openevse-" + lowercased shortId.
std::string mqtt_default_base(const std::string &shortId);
```

### 2. `lite_mqtt.{h,cpp}` — device glue (`#ifdef OPENEVSE_LITE`)
Owns the `WiFiClient` + `PubSubClient`. NOT in the native build filter.

```cpp
class LiteMqtt {
public:
  void begin(const LiteMqttConfig &cfg, const String &shortId,
             const String &fwVersion);   // store cfg/id/fw; sets server/port/LWT; no blocking connect
                                          // announce JSON is built internally at connect
                                          // (shortId + fwVersion + WiFi.localIP())
  void reconfigure(const LiteMqttConfig &cfg);  // apply changed config live (drops + reconnects)
  // Pumps PubSubClient, reconnects with backoff, and publishes per-field on cadence.
  // statusFn fills a JsonDocument with the status fields to publish.
  void loop(uint32_t nowMs, bool charging,
            void (*statusFn)(JsonDocument &));
  bool connected() const;
private:
  // computes base via cfg.topic || mqtt_default_base(shortId)
  // on connect: publish announce (retained) + available=online (retained)
  // publish: iterate JsonObject root, publish mqtt_topic_join(base,key)=value (retained)
};
```

- Buffer: `setBufferSize(512)` at begin (announce JSON can exceed the 256 B default;
  per-field messages are tiny).
- LWT: `connect(clientId, user, pass, willTopic=base/available, willQos=0,
  willRetain=true, willMessage="offline")`. On success publish `available=online`
  retained, then the announce.
- Client id: `"juicebox-" + shortId`.
- Disabled config (`enabled == false`) -> `loop()` is a no-op and ensures disconnected.

### 3. Config — `LiteMqttConfig` in `lite_config_store.{h,cpp}`
```cpp
struct LiteMqttConfig {
  bool     enabled;   // mqtt_enabled ("me")  default false
  String   server;    // mqtt_server          default ""
  int      port;      // mqtt_port            default 1883
  String   topic;     // mqtt_topic (base)    default "" -> mqtt_default_base(shortId)
  String   user;      // mqtt_user            default ""
  String   pass;      // mqtt_pass            default ""
  uint32_t period_s;  // idle publish period  default 30  (charging period fixed = 5)
};
bool lite_config_load_mqtt(LiteMqttConfig &out);  // false if no key -> caller uses defaults
bool lite_config_save_mqtt(const LiteMqttConfig &in);
```
Persisted **per-key** via the existing `kv_set_str`/`kv_get_str` + `kv_set_int`/
`kv_get_int` helpers (the `String`-bearing pattern used by `wifi`/`clock` configs —
a `fdb_blob` can't serialize `String` members). Keys: `mqtt_enabled` (int 0/1),
`mqtt_server`, `mqtt_port` (int), `mqtt_topic`, `mqtt_user`, `mqtt_pass`,
`mqtt_period` (int). Key *names* mirror upstream `app_config` so any later lift lines up.

### 4. Status doc reuse (DRY) — `web_server_lite.{h,cpp}`
`build_status_json(String&)` currently builds a `StaticJsonDocument<1280>` then
serializes. Extract the doc-building into a reusable function and expose it:

```cpp
// in web_server_lite.h
void web_server_lite_build_status(JsonDocument &doc);
```
`build_status_json` becomes: build into the doc via this function, then
`serializeJson`. No behavior change to `/status`. `LiteMqtt::loop` passes this as
`statusFn` so the published field set always matches `/status`.

Scalars only are published (string/int/float/bool). Nested objects/arrays, if any,
are skipped (none today; guard defensively).

### 5. Loop wiring — `web_server_lite.cpp` (NOT main)
The divert/shaper config + runtime loops already live in `web_server_lite.cpp`
(`s_divertCfg`, `s_shaperCfg`, driven by `web_server_lite_loop`). MQTT needs the same
file's resources (config statics, `build_status_json`, `s_mgr_ctrl->isCharging()`,
`LITE_FW_VERSION`, `ESPAL`), so it lives there too — no cross-file plumbing.
- File-statics `LiteMqtt s_mqtt;` and `LiteMqttConfig s_mqttCfg;`.
- `web_server_lite_begin(...)`: `lite_config_load_mqtt(s_mqttCfg)` (defaults if absent),
  then `s_mqtt.begin(s_mqttCfg, ESPAL.getShortId(), LITE_FW_VERSION)`.
- `web_server_lite_loop()` (end): `s_mqtt.loop(millis(),
  s_mgr_ctrl && s_mgr_ctrl->isCharging(), &web_server_lite_build_status);`
- Constants (top of `lite_mqtt.cpp`): `LITE_MQTT_CHARGE_MS = 5000`,
  `LITE_MQTT_BACKOFF_MS = 5000`; idle period comes from `cfg.period_s * 1000`
  (default 30 s when `period_s == 0`).
- `web_server_lite_build_status(JsonDocument&)` is a non-static free function (declared
  in `web_server_lite.h`, needs `#include <ArduinoJson.h>` there) so it can be passed as
  the `statusFn` pointer.

### 6. `/config` integration — `web_server_lite.cpp`
- `config_json`: emit `mqtt_enabled`, `mqtt_server`, `mqtt_port`, `mqtt_topic`,
  `mqtt_user`, `mqtt_period`. **Password is never echoed** — emit `mqtt_pass` as `""`
  (presence only) or omit; a POST with empty `mqtt_pass` leaves the stored pass
  unchanged (so re-saving the form doesn't wipe it).
- `handle_config` parse block (mirrors the divert/shaper blocks):
  `qarg("mqtt_enabled"...) ... lite_config_save_mqtt(mcfg)` then
  `s_mqtt.reconfigure(mcfg)` to apply live. A `mqtt_pass` arg that is present and
  non-empty replaces the stored password; absent/empty preserves it.

## Data flow

```
build_status_json ─┐
                   ├─ web_server_lite_build_status(doc)
GET /status ───────┘            │
                                ▼
LiteMqtt::loop ── statusFn(doc) ── iterate scalars ── PubSubClient.publish(
                                                        base/<key>, value, retained)
                                                      ── over WiFiClient (plaintext TCP)
```

## Error handling

- **Broker down / connect fails:** `loop()` retries on `mqtt_backoff_due` (5 s). Never
  blocks the main loop (single `connect()` attempt is the only blocking call; PubSubClient
  connect timeout is its socket default — acceptable, runs on the main task like the
  WebServer already does).
- **WiFi down:** `WiFi.status() != WL_CONNECTED` -> skip connect attempts; the AP-recovery
  path in `loop()` already handles re-association.
- **Disabled:** no socket opened; `loop()` early-returns.
- **Publish failure:** ignored for that cycle; next cadence tick retries. No buffering.
- **Config change:** `reconfigure` disconnects cleanly (publishes nothing) and the next
  loop reconnects with new settings, re-emitting retained announce + availability.

## Testing

**Native doctest — `test/test_lite_mqtt/`** (pure `lite_mqtt_policy`):
- `mqtt_should_publish`: never-published is due; not due before period; due at/after
  period; charging uses the shorter period; unsigned wrap (now < last) treated as due.
- `mqtt_backoff_due`: zero last is due; before/after backoff.
- `mqtt_topic_join`: normal join; trailing slash on base; leading slash on key; both;
  empty base; empty key.
- `mqtt_default_base`: lowercases + prefixes `openevse-`.

**Device build verification** (per task): `pio run -e openevse_lite` succeeds; flash
stays well under the 960 KB OTA slot (baseline 57.9%; PubSubClient adds a few KB).

**HW round-trip (deferred to user bench):** point at a broker, confirm `mosquitto_sub -t
'openevse-XXXX/#'` shows per-field telemetry, faster cadence while charging, `available`
flips `online`/`offline` on connect/power-off (LWT), and `/config` round-trips settings
(password preserved on empty re-save).

## File summary

| File | Change |
|------|--------|
| `src/lite/lite_mqtt_policy.h` | Create — pure decl |
| `src/lite/lite_mqtt_policy.cpp` | Create — pure impl |
| `src/lite/lite_mqtt.h` | Create — `LiteMqtt` decl |
| `src/lite/lite_mqtt.cpp` | Create — PubSubClient glue |
| `src/lite/lite_config_store.h` | Add `LiteMqttConfig` + load/save decls |
| `src/lite/lite_config_store.cpp` | Add load/save (FlashDB blob `"mqtt"`) |
| `src/lite/web_server_lite.h` | `#include <ArduinoJson.h>`; add `web_server_lite_build_status` decl |
| `src/lite/web_server_lite.cpp` | Extract status doc builder; `s_mqtt`+`s_mqttCfg`; begin/loop wiring; `/config` mqtt fields |
| `platformio.ini` | `lib_deps += knolleary/PubSubClient`; native filter += policy |
| `test/test_lite_mqtt/` | Create — native doctest for policy |

## Constraints carried from project memory

- Commit author `Andrew Rankin <andrew@eiknet.com>`, no Claude attribution.
- Push only to `origin` (RAR fork) `main`.
- Never commit real WiFi/broker creds.
- Lite is dual-OTA; real ceiling is the 960 KB ota slot (PlatformIO's % is ~half true).
- Flash via `scripts/lite_flash.sh` (bootloader@0x0 + app@0x8000); never raw_firmware.elf.
