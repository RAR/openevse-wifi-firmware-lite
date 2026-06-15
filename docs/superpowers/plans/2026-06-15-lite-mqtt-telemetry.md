# JuiceBox-Lite MQTT Telemetry Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an outbound-only, plaintext MQTT telemetry publisher to the JuiceBox-lite firmware that mirrors the standard OpenEVSE per-field topic convention (`<base>/<key>`).

**Architecture:** A pure cadence/topic policy unit (host-tested) + a device `LiteMqtt` class wrapping `knolleary/PubSubClient` over LibreTiny `WiFiClient`. The publisher lives in `web_server_lite.cpp` alongside the existing divert/shaper config + loops, reusing the `/status` document builder (DRY). Config is a per-key FlashDB record, readable/writable through the existing `/config` endpoint. Telemetry-only: no subscriptions, no control.

**Tech Stack:** C++17, LibreTiny (silabs-efm32gg11), `knolleary/PubSubClient`, ArduinoJson 6.20.1, FlashDB KVS, doctest (native).

**Reference spec:** `docs/superpowers/specs/2026-06-15-lite-mqtt-telemetry-design.md`

**Project constraints (from memory):**
- Commit author `Andrew Rankin <andrew@eiknet.com>`, NO Claude attribution. Use:
  `git -c user.name="Andrew Rankin" -c user.email="andrew@eiknet.com" commit ...`
- Device build: `pio run -e openevse_lite`. Native tests: `pio test -e native`.
- Never commit real broker/WiFi creds. Push only to `origin` (RAR fork) `main`.
- HW flash/round-trip is the user's bench — deferred (Task 6 documents it, does not run it).

---

## File Structure

| File | Responsibility |
|------|----------------|
| `src/lite/lite_mqtt_policy.h/.cpp` | Pure cadence + topic helpers (no I/O), native-tested |
| `src/lite/lite_mqtt.h/.cpp` | `LiteMqtt` device class: PubSubClient + WiFiClient glue |
| `src/lite/lite_config_store.h/.cpp` | `LiteMqttConfig` + per-key load/save |
| `src/lite/web_server_lite.h/.cpp` | Expose status doc builder; host `s_mqtt`; `/config` fields |
| `platformio.ini` | `PubSubClient` dep (device); policy in native filter |
| `test/test_lite_mqtt/test_lite_mqtt_policy.cpp` | doctest for the policy unit |

---

## Task 1: Pure MQTT policy unit (cadence + topic helpers)

**Files:**
- Create: `src/lite/lite_mqtt_policy.h`
- Create: `src/lite/lite_mqtt_policy.cpp`
- Create: `test/test_lite_mqtt/test_lite_mqtt_policy.cpp`
- Modify: `platformio.ini` (`[env:native]` `build_src_filter`)

- [ ] **Step 1: Write the failing test**

Create `test/test_lite_mqtt/test_lite_mqtt_policy.cpp`:

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../src/lite/lite_mqtt_policy.h"

TEST_CASE("should_publish: never-published is always due") {
  CHECK(mqtt_should_publish(0, 0,     30000, 5000, false) == true);
  CHECK(mqtt_should_publish(0, 12345, 30000, 5000, false) == true);
}
TEST_CASE("should_publish: idle period gating") {
  CHECK(mqtt_should_publish(1000, 1000 + 29999, 30000, 5000, false) == false);
  CHECK(mqtt_should_publish(1000, 1000 + 30000, 30000, 5000, false) == true);
}
TEST_CASE("should_publish: charging uses the shorter period") {
  CHECK(mqtt_should_publish(1000, 1000 + 4999, 30000, 5000, true) == false);
  CHECK(mqtt_should_publish(1000, 1000 + 5000, 30000, 5000, true) == true);
  // same elapsed, idle would NOT be due:
  CHECK(mqtt_should_publish(1000, 1000 + 5000, 30000, 5000, false) == false);
}
TEST_CASE("should_publish: unsigned wrap is due") {
  uint32_t last = 0xFFFFF000u, now = 0x00000400u; // delta 0x1400 = 5120
  CHECK(mqtt_should_publish(last, now, 30000, 5000, true)  == true);  // >= 5000
  CHECK(mqtt_should_publish(last, now, 30000, 5000, false) == false); // < 30000
}
TEST_CASE("backoff_due") {
  CHECK(mqtt_backoff_due(0, 0, 5000)          == true);   // never attempted
  CHECK(mqtt_backoff_due(1000, 1000+4999, 5000) == false);
  CHECK(mqtt_backoff_due(1000, 1000+5000, 5000) == true);
}
TEST_CASE("topic_join: single slash, trims extras") {
  CHECK(mqtt_topic_join("openevse", "amp")    == "openevse/amp");
  CHECK(mqtt_topic_join("openevse/", "amp")   == "openevse/amp");
  CHECK(mqtt_topic_join("openevse", "/amp")   == "openevse/amp");
  CHECK(mqtt_topic_join("openevse/", "/amp")  == "openevse/amp");
  CHECK(mqtt_topic_join("", "amp")            == "amp");
  CHECK(mqtt_topic_join("openevse", "")       == "openevse");
}
TEST_CASE("default_base: lowercases and prefixes") {
  CHECK(mqtt_default_base("A1B2C3") == "openevse-a1b2c3");
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `pio test -e native -f test_lite_mqtt`
Expected: FAIL — `lite_mqtt_policy.h` not found / functions undefined.

- [ ] **Step 3: Write the header**

Create `src/lite/lite_mqtt_policy.h`:

```cpp
#pragma once
#include <stdint.h>
#include <string>

// Pure MQTT telemetry decision logic (no Arduino, no PubSubClient, no sockets) —
// native-testable. Time args are millis()-style uint32_t; deltas are wrap-safe.

// Publish cadence. period = charging ? chargeMs : idleMs.
//   lastMs == 0            -> true (never published yet)
//   (now - last) >= period -> true
//   else                   -> false
bool mqtt_should_publish(uint32_t lastMs, uint32_t nowMs,
                         uint32_t idleMs, uint32_t chargeMs, bool charging);

// Reconnect backoff. true when lastAttemptMs == 0 or (now - lastAttempt) >= backoffMs.
bool mqtt_backoff_due(uint32_t lastAttemptMs, uint32_t nowMs, uint32_t backoffMs);

// Join "<base>/<key>" with exactly one slash: trims trailing '/' from base and
// leading '/' from key. Empty base -> key; empty key -> base.
std::string mqtt_topic_join(const std::string &base, const std::string &key);

// Default base when configured topic is blank: "openevse-" + lowercase(shortId).
std::string mqtt_default_base(const std::string &shortId);
```

- [ ] **Step 4: Write the implementation**

Create `src/lite/lite_mqtt_policy.cpp`:

```cpp
#include "lite_mqtt_policy.h"

bool mqtt_should_publish(uint32_t lastMs, uint32_t nowMs,
                         uint32_t idleMs, uint32_t chargeMs, bool charging)
{
  if (lastMs == 0) return true;
  uint32_t period = charging ? chargeMs : idleMs;
  return (uint32_t)(nowMs - lastMs) >= period;
}

bool mqtt_backoff_due(uint32_t lastAttemptMs, uint32_t nowMs, uint32_t backoffMs)
{
  if (lastAttemptMs == 0) return true;
  return (uint32_t)(nowMs - lastAttemptMs) >= backoffMs;
}

std::string mqtt_topic_join(const std::string &base, const std::string &key)
{
  std::string b = base, k = key;
  while (!b.empty() && b.back() == '/')  b.pop_back();
  size_t i = 0;
  while (i < k.size() && k[i] == '/')    ++i;
  k = k.substr(i);
  if (b.empty()) return k;
  if (k.empty()) return b;
  return b + "/" + k;
}

std::string mqtt_default_base(const std::string &shortId)
{
  std::string s = shortId;
  for (char &c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  return "openevse-" + s;
}
```

- [ ] **Step 5: Add the policy unit to the native build filter**

In `platformio.ini`, `[env:native]` `build_src_filter`, append ` +<lite/lite_mqtt_policy.cpp>` to the end of the existing filter line (after `+<lite/lite_provision.cpp>`).

- [ ] **Step 6: Run to verify it passes**

Run: `pio test -e native -f test_lite_mqtt`
Expected: PASS — all cases in `test_lite_mqtt_policy.cpp`.

- [ ] **Step 7: Commit**

```bash
git add src/lite/lite_mqtt_policy.h src/lite/lite_mqtt_policy.cpp \
        test/test_lite_mqtt/test_lite_mqtt_policy.cpp platformio.ini
git -c user.name="Andrew Rankin" -c user.email="andrew@eiknet.com" \
  commit -m "feat(lite): pure MQTT cadence + topic policy unit (native-tested)"
```

---

## Task 2: `LiteMqttConfig` + per-key persistence

**Files:**
- Modify: `src/lite/lite_config_store.h` (add struct + decls)
- Modify: `src/lite/lite_config_store.cpp` (add load/save)

No native test — FlashDB is device-only. Verified by the device build.

- [ ] **Step 1: Add the struct + declarations**

In `src/lite/lite_config_store.h`, after the shaper block (`lite_config_save_shaper` decl) and before the clock block, add:

```cpp
// MQTT telemetry config. Persisted PER-KEY (mqtt_enabled/mqtt_server/mqtt_port/
// mqtt_topic/mqtt_user/mqtt_pass/mqtt_period) because String members can't go in a
// fdb_blob — same pattern as wifi/clock. Key names mirror upstream app_config.
struct LiteMqttConfig {
  bool     enabled;   // default false
  String   server;    // broker host/IP, default ""
  int      port;      // default 1883
  String   topic;     // base topic; "" -> mqtt_default_base(shortId) at runtime
  String   user;      // default ""
  String   pass;      // default ""
  uint32_t period_s;  // idle publish period seconds, default 30
};
bool lite_config_load_mqtt(LiteMqttConfig &out);  // fills defaults; true if any key present
bool lite_config_save_mqtt(const LiteMqttConfig &in);
```

- [ ] **Step 2: Add load/save**

In `src/lite/lite_config_store.cpp`, after the clock load/save functions (before the final `#endif`), add:

```cpp
// --- MQTT telemetry config (per-key; String members preclude a blob) ---

bool lite_config_load_mqtt(LiteMqttConfig &out)
{
  out.enabled  = false;          // defaults
  out.server   = "";
  out.port     = 1883;
  out.topic    = "";
  out.user     = "";
  out.pass     = "";
  out.period_s = 30;
  if (!s_ready) return false;
  int en = 0, port = 1883, period = 30;
  bool any = false;
  if (kv_get_int("mqtt_enabled", en)) { out.enabled = (en != 0); any = true; }
  { String s; if (kv_get_str("mqtt_server", s)) { out.server = s; any = true; } }
  if (kv_get_int("mqtt_port", port))   { out.port = port; any = true; }
  { String s; if (kv_get_str("mqtt_topic", s)) { out.topic = s; any = true; } }
  { String s; if (kv_get_str("mqtt_user",  s)) { out.user = s;  any = true; } }
  { String s; if (kv_get_str("mqtt_pass",  s)) { out.pass = s;  any = true; } }
  if (kv_get_int("mqtt_period", period)) { out.period_s = (uint32_t)period; any = true; }
  return any;
}

bool lite_config_save_mqtt(const LiteMqttConfig &in)
{
  if (!s_ready) return false;
  bool ok = kv_set_int("mqtt_enabled", in.enabled ? 1 : 0);
  ok = kv_set_str("mqtt_server", in.server)            && ok;
  ok = kv_set_int("mqtt_port",   in.port)              && ok;
  ok = kv_set_str("mqtt_topic",  in.topic)             && ok;
  ok = kv_set_str("mqtt_user",   in.user)              && ok;
  ok = kv_set_str("mqtt_pass",   in.pass)              && ok;
  ok = kv_set_int("mqtt_period", (int)in.period_s)     && ok;
  return ok;
}
```

> Note: `kv_get_str`/`kv_set_str`/`kv_get_int`/`kv_set_int` and the `s_ready` flag are the existing file-local helpers used by the wifi/clock/evse functions. Match their exact spelling by reading the top of `lite_config_store.cpp` first.

- [ ] **Step 3: Verify the device build**

Run: `pio run -e openevse_lite`
Expected: SUCCESS (no new behavior yet; just confirms it compiles).

- [ ] **Step 4: Commit**

```bash
git add src/lite/lite_config_store.h src/lite/lite_config_store.cpp
git -c user.name="Andrew Rankin" -c user.email="andrew@eiknet.com" \
  commit -m "feat(lite): LiteMqttConfig + per-key FlashDB persistence"
```

---

## Task 3: Expose the status doc builder (DRY refactor, no behavior change)

**Files:**
- Modify: `src/lite/web_server_lite.h` (include ArduinoJson; declare builder)
- Modify: `src/lite/web_server_lite.cpp` (extract builder from `build_status_json`)

- [ ] **Step 1: Declare the builder in the header**

In `src/lite/web_server_lite.h`, inside the `#ifdef OPENEVSE_LITE` guard, add an ArduinoJson include and the declaration:

```cpp
#include <ArduinoJson.h>
// ... existing forward decls + web_server_lite_begin/loop ...
// Fill `doc` with the same fields /status serializes (shared by /status + MQTT telemetry).
void web_server_lite_build_status(JsonDocument &doc);
```

- [ ] **Step 2: Extract the doc-building body**

In `src/lite/web_server_lite.cpp`, change `static void build_status_json(String &out)` so the document construction moves into a new non-static function and `build_status_json` just serializes:

```cpp
void web_server_lite_build_status(JsonDocument &doc)
{
  // (entire current body of build_status_json EXCEPT the StaticJsonDocument decl
  //  and the trailing serializeJson — i.e. all the doc[...] = ... assignments,
  //  the if (s_mgr_ctrl){...}, identity/system block, clock, totals, feed echoes)
}

static void build_status_json(String &out)
{
  StaticJsonDocument<1280> doc;
  web_server_lite_build_status(doc);
  serializeJson(doc, out);
}
```

Mechanically: take lines 313–389 of the current `build_status_json` (the `StaticJsonDocument<1280> doc;` line stays in `build_status_json`; everything from `if (s_mgr_ctrl) {` through the feed-echo block moves into `web_server_lite_build_status` operating on the passed-in `doc`; the `serializeJson(doc, out);` stays in `build_status_json`). No field changes.

- [ ] **Step 3: Verify the device build + unchanged /status**

Run: `pio run -e openevse_lite`
Expected: SUCCESS. (`/status` output is byte-identical — same fields, same order.)

- [ ] **Step 4: Commit**

```bash
git add src/lite/web_server_lite.h src/lite/web_server_lite.cpp
git -c user.name="Andrew Rankin" -c user.email="andrew@eiknet.com" \
  commit -m "refactor(lite): expose web_server_lite_build_status (shared by /status + MQTT)"
```

---

## Task 4: `LiteMqtt` device module + PubSubClient dep + loop wiring

**Files:**
- Create: `src/lite/lite_mqtt.h`
- Create: `src/lite/lite_mqtt.cpp`
- Modify: `platformio.ini` (`[env:openevse_lite]` `lib_deps`)
- Modify: `src/lite/web_server_lite.cpp` (instance + begin + loop call)

- [ ] **Step 1: Add the PubSubClient dependency**

In `platformio.ini`, `[env:openevse_lite]` `lib_deps`, add a line:

```
  knolleary/PubSubClient@^2.8
```

(Leave the `[env:native]` `lib_deps` untouched — the policy unit has no MQTT deps.)

- [ ] **Step 2: Write the class header**

Create `src/lite/lite_mqtt.h`:

```cpp
#pragma once
#ifdef OPENEVSE_LITE
#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFiClient.h>
#include <PubSubClient.h>
#include "lite_config_store.h"

// Outbound-only MQTT telemetry. Plaintext (WiFiClient). Per-field <base>/<key>
// publishes for OpenEVSE-MQTT parity. No subscriptions / control. Non-blocking:
// connect is attempted on a backoff inside loop(), which runs on the main task.
class LiteMqtt {
public:
  void begin(const LiteMqttConfig &cfg, const String &shortId, const String &fwVersion);
  void reconfigure(const LiteMqttConfig &cfg);   // apply live: disconnect, re-point server
  // Pump the client, reconnect on backoff, and publish per-field on cadence.
  // statusFn fills a JsonDocument with the fields to publish (pass
  // web_server_lite_build_status). No-op when disabled or WiFi is down.
  void loop(uint32_t nowMs, bool charging, void (*statusFn)(JsonDocument &));
  bool connected();
private:
  String baseTopic() const;       // cfg.topic or mqtt_default_base(shortId)
  bool   connectNow();            // single connect attempt + announce + availability
  void   publishAnnounce();
  void   publishFields(JsonDocument &doc);

  WiFiClient   _net;
  PubSubClient _mqtt{_net};
  LiteMqttConfig _cfg;
  String   _shortId;
  String   _fw;
  uint32_t _lastAttempt = 0;
  uint32_t _lastPublish = 0;
};
#endif
```

- [ ] **Step 3: Write the implementation**

Create `src/lite/lite_mqtt.cpp`:

```cpp
#ifdef OPENEVSE_LITE
#include "lite_mqtt.h"
#include "lite_mqtt_policy.h"
#include <WiFi.h>

static const uint32_t LITE_MQTT_CHARGE_MS  = 5000;
static const uint32_t LITE_MQTT_BACKOFF_MS = 5000;
static const uint16_t LITE_MQTT_BUF        = 512;   // announce JSON exceeds the 256 default

void LiteMqtt::begin(const LiteMqttConfig &cfg, const String &shortId, const String &fwVersion)
{
  _cfg = cfg;
  _shortId = shortId;
  _fw = fwVersion;
  _mqtt.setBufferSize(LITE_MQTT_BUF);
  if (_cfg.server.length()) _mqtt.setServer(_cfg.server.c_str(), _cfg.port);
}

void LiteMqtt::reconfigure(const LiteMqttConfig &cfg)
{
  _cfg = cfg;
  if (_mqtt.connected()) _mqtt.disconnect();
  _lastAttempt = 0;   // allow an immediate reconnect with new settings
  if (_cfg.server.length()) _mqtt.setServer(_cfg.server.c_str(), _cfg.port);
}

bool LiteMqtt::connected() { return _mqtt.connected(); }

String LiteMqtt::baseTopic() const
{
  if (_cfg.topic.length()) return _cfg.topic;
  return String(mqtt_default_base(std::string(_shortId.c_str())).c_str());
}

bool LiteMqtt::connectNow()
{
  if (!_cfg.server.length()) return false;
  String clientId = String("juicebox-") + _shortId;
  String avail = String(mqtt_topic_join(std::string(baseTopic().c_str()), "available").c_str());

  bool ok;
  if (_cfg.user.length()) {
    ok = _mqtt.connect(clientId.c_str(), _cfg.user.c_str(), _cfg.pass.c_str(),
                       avail.c_str(), 0, true, "offline");
  } else {
    ok = _mqtt.connect(clientId.c_str(), nullptr, nullptr,
                       avail.c_str(), 0, true, "offline");
  }
  if (ok) {
    _mqtt.publish(avail.c_str(), "online", true);
    publishAnnounce();
  }
  return ok;
}

void LiteMqtt::publishAnnounce()
{
  StaticJsonDocument<192> a;
  a["firmware"] = _fw;
  a["id"]       = _shortId;
  IPAddress ip = WiFi.localIP();
  char ipbuf[16];
  snprintf(ipbuf, sizeof(ipbuf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  a["ipaddress"] = ipbuf;
  a["http"]      = String("http://") + ipbuf + "/";
  String out; serializeJson(a, out);
  String t = String(mqtt_topic_join(std::string(baseTopic().c_str()), "announce").c_str());
  _mqtt.publish(t.c_str(), out.c_str(), true);
}

void LiteMqtt::publishFields(JsonDocument &doc)
{
  std::string base = std::string(baseTopic().c_str());
  for (JsonPair kv : doc.as<JsonObject>()) {
    if (kv.value().is<JsonObject>() || kv.value().is<JsonArray>()) continue;  // scalars only
    std::string topic = mqtt_topic_join(base, std::string(kv.key().c_str()));
    String val = kv.value().as<String>();
    _mqtt.publish(topic.c_str(), val.c_str(), true);   // retained
  }
}

void LiteMqtt::loop(uint32_t nowMs, bool charging, void (*statusFn)(JsonDocument &))
{
  if (!_cfg.enabled) {
    if (_mqtt.connected()) _mqtt.disconnect();
    return;
  }
  if (WiFi.status() != WL_CONNECTED) return;

  if (!_mqtt.connected()) {
    if (!mqtt_backoff_due(_lastAttempt, nowMs, LITE_MQTT_BACKOFF_MS)) return;
    _lastAttempt = nowMs;
    if (!connectNow()) return;
    _lastPublish = 0;   // publish immediately after a fresh connect
  }

  _mqtt.loop();

  uint32_t idleMs = _cfg.period_s ? _cfg.period_s * 1000u : 30000u;
  if (mqtt_should_publish(_lastPublish, nowMs, idleMs, LITE_MQTT_CHARGE_MS, charging)) {
    _lastPublish = nowMs;
    StaticJsonDocument<1280> doc;
    statusFn(doc);
    publishFields(doc);
  }
}
#endif
```

- [ ] **Step 4: Instantiate + wire begin/loop in the web server**

In `src/lite/web_server_lite.cpp`:

1. Add include near the other lite includes: `#include "lite_mqtt.h"`
2. Add file-statics near `s_shaperCfg` (~line 90):

```cpp
static LiteMqtt       s_mqtt;
static LiteMqttConfig s_mqttCfg;
```

3. In `web_server_lite_begin(...)`, after the existing config loads (where `s_divertCfg`/`s_shaperCfg` are loaded), add:

```cpp
lite_config_load_mqtt(s_mqttCfg);
s_mqtt.begin(s_mqttCfg, ESPAL.getShortId(), LITE_FW_VERSION);
```

4. In `web_server_lite_loop()`, at the very end of the function, add:

```cpp
s_mqtt.loop(millis(), s_mgr_ctrl && s_mgr_ctrl->isCharging(),
            &web_server_lite_build_status);
```

> Confirm `LiteEvseManager` exposes `isCharging()` (it is called as `s_manager.isCharging()` in `main_lite.cpp`) and that `ESPAL` + `LITE_FW_VERSION` are already in scope in this file (both are used by `build_status_json`).

- [ ] **Step 5: Verify the device build + flash budget**

Run: `pio run -e openevse_lite`
Expected: SUCCESS. Note the `Flash: [...] N%` line; confirm firmware.bin is still well under the 960 KB OTA slot:
Run: `printf '%.1f%%\n' "$(echo "scale=4;$(wc -c < .pio/build/openevse_lite/firmware.bin)/983040*100" | bc)"`
Expected: roughly 58–60% (baseline 57.9% + a few KB for PubSubClient).

- [ ] **Step 6: Commit**

```bash
git add src/lite/lite_mqtt.h src/lite/lite_mqtt.cpp platformio.ini src/lite/web_server_lite.cpp
git -c user.name="Andrew Rankin" -c user.email="andrew@eiknet.com" \
  commit -m "feat(lite): LiteMqtt telemetry publisher (PubSubClient) wired into web loop"
```

---

## Task 5: `/config` read + write of MQTT settings

**Files:**
- Modify: `src/lite/web_server_lite.cpp` (`config_json` + `handle_config`)

- [ ] **Step 1: Emit MQTT fields in `config_json` (password NOT echoed)**

In `config_json` (after the shaper fields, before `serializeJson`), add:

```cpp
doc["mqtt_enabled"] = s_mqttCfg.enabled;
doc["mqtt_server"]  = s_mqttCfg.server;
doc["mqtt_port"]    = s_mqttCfg.port;
doc["mqtt_topic"]   = s_mqttCfg.topic;
doc["mqtt_user"]    = s_mqttCfg.user;
doc["mqtt_period"]  = s_mqttCfg.period_s;
// mqtt_pass intentionally NOT echoed; a present non-empty mqtt_pass on POST replaces it.
```

> The `config_json` doc is `StaticJsonDocument<384>`. Adding six fields will overflow it — bump it to `StaticJsonDocument<640>` at the top of `config_json`.

- [ ] **Step 2: Parse MQTT fields in `handle_config`**

In `handle_config`, after the shaper parse block (`if (sany) { ... }`), add:

```cpp
LiteMqttConfig mcfg = s_mqttCfg; bool many = false;
if (qarg("mqtt_enabled", v)) { mcfg.enabled = v.toInt() != 0; many = true; }
if (qarg("mqtt_server", v))  { mcfg.server  = v;              many = true; }
if (qarg("mqtt_port", v))    { mcfg.port    = v.toInt();      many = true; }
if (qarg("mqtt_topic", v))   { mcfg.topic   = v;              many = true; }
if (qarg("mqtt_user", v))    { mcfg.user    = v;              many = true; }
if (qarg("mqtt_period", v))  { mcfg.period_s = (uint32_t)v.toInt(); many = true; }
// Password: only overwrite when a non-empty value is supplied (qarg already
// requires length > 0), so re-saving the form with a blank pass preserves it.
if (qarg("mqtt_pass", v))    { mcfg.pass    = v;              many = true; }
if (many) { lite_config_save_mqtt(mcfg); s_mqttCfg = mcfg; s_mqtt.reconfigure(mcfg); }
```

- [ ] **Step 3: Verify the device build**

Run: `pio run -e openevse_lite`
Expected: SUCCESS.

- [ ] **Step 4: Commit**

```bash
git add src/lite/web_server_lite.cpp
git -c user.name="Andrew Rankin" -c user.email="andrew@eiknet.com" \
  commit -m "feat(lite): /config reads+writes MQTT settings (password preserved on blank)"
```

---

## Task 6: Final verification + flash-ready hand-off (HW deferred)

**Files:** none (verification only).

- [ ] **Step 1: Full native suite**

Run: `pio test -e native`
Expected: all suites PASS, including `test_lite_mqtt`.

- [ ] **Step 2: Clean device build**

Run: `pio run -e openevse_lite`
Expected: SUCCESS; record the Flash % and firmware.bin size vs the 960 KB OTA slot.

- [ ] **Step 3: Document the deferred HW round-trip**

The user's bench performs (do NOT attempt remotely):
1. Flash: `pio run -e openevse_lite -t upload` (bootloader@0x0 + app@0x8000).
2. `POST /config` with `mqtt_enabled=1&mqtt_server=<broker>&mqtt_user=...&mqtt_pass=...`.
3. `mosquitto_sub -h <broker> -t 'openevse-<id>/#' -v` — confirm per-field topics,
   30 s idle cadence, 5 s while charging, and `available` = `online`.
4. Power-cycle the unit — confirm LWT publishes `available` = `offline` (retained).
5. Re-`POST /config` without `mqtt_pass` — confirm broker auth still works (pass preserved).

- [ ] **Step 4: Final commit (if any verification notes/docs were added)**

No code changes expected here. If notes were added to the spec, commit them; otherwise this task is just the green-light gate before pushing.

```bash
git push origin main
```

---

## Self-Review

**Spec coverage:**
- Per-field `<base>/<key>` publish → Task 4 `publishFields`. ✓
- 30 s idle / 5 s charging cadence → Task 1 `mqtt_should_publish` + Task 4 `loop`. ✓
- LWT availability `online`/`offline` → Task 4 `connectNow` (will) + on-connect publish. ✓
- Retained announce → Task 4 `publishAnnounce`. ✓
- `LiteMqttConfig` per-key persistence → Task 2. ✓
- `/config` GET+POST, password preserved → Task 5. ✓
- DRY status doc reuse → Task 3. ✓
- PubSubClient over WiFiClient, plaintext, non-blocking backoff → Task 4. ✓
- Telemetry-only (no subscriptions) → Task 4 (no `subscribe`/`setCallback`). ✓
- Native tests for pure logic → Task 1. ✓
- Out-of-scope (TLS, discovery, control, web page) → not implemented. ✓

**Type consistency:** `LiteMqttConfig` fields (`enabled/server/port/topic/user/pass/period_s`) identical across Tasks 2/4/5. `mqtt_should_publish`/`mqtt_backoff_due`/`mqtt_topic_join`/`mqtt_default_base` signatures identical in Task 1 header, tests, and Task 4 calls. `web_server_lite_build_status(JsonDocument&)` consistent in Task 3 decl/defn and Task 4 call. ✓

**Placeholder scan:** none — every code step has complete code. ✓
