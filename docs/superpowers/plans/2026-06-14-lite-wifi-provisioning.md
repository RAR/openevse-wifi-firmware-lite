# JuiceBox-lite WiFi Provisioning + UI Serving — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the lite firmware's compile-time WiFi creds + plain-text `GET /` with a stored-cred + softAP provisioning flow that serves two gzipped GUI bundles (runtime app in STA mode, setup page in AP mode).

**Architecture:** A pure native-tested decision unit (`lite_provision`) drives a boot/runtime WiFi state machine (`STA_CONNECTING` → `STA_ONLINE` / `AP_PROVISION`); thin device glue in `main_lite.cpp` does the radio and `web_server_lite.cpp` does the HTTP serving + `/scan` + `/connect`. Stored-only creds (no build-flag path); AP on no-creds-or-connect-fail with periodic STA auto-retry.

**Tech Stack:** LibreTiny (silabs-efm32gg11 / WF200), Arduino WiFi API (softAP/scan available), Mongoose 6.x (`MongooseLite`), ArduinoJson, FlashDB cred store, doctest native tests.

**Reference:** spec `docs/superpowers/specs/2026-06-14-lite-wifi-provisioning-design.md`. Locked decisions D1–D5 there. `/scan` + `/connect` shapes are FINAL.

---

## File Structure

- **Create:** `src/lite/lite_provision.h` / `lite_provision.cpp` — pure decision unit (no I/O).
- **Create:** `test/test_lite_provision/test_lite_provision.cpp` — doctest suite.
- **Create:** `src/lite/web_ui_lite.h` — generated/placeholder gzipped bundle bytes (`INDEX_HTML_GZ[]`, `SETUP_HTML_GZ[]` + lengths).
- **Modify:** `src/lite/main_lite.cpp` — boot rework (config-first, bounded connect, AP fallback, retry); remove build-flag cred path.
- **Modify:** `src/lite/web_server_lite.cpp` — `s_apMode` + mode-aware `GET /` + `/scan` + `/connect` + deferred-reboot hook.
- **Modify:** `platformio.ini` — add `lite_provision.cpp` to `[env:native]` `build_src_filter`; remove `LITE_WIFI_SSID/PASS` + `*_DEFAULT` from `[env:openevse_lite]`.

---

## Task 1: `lite_provision` pure decision unit (TDD, native)

**Files:**
- Create: `src/lite/lite_provision.h`, `src/lite/lite_provision.cpp`
- Test: `test/test_lite_provision/test_lite_provision.cpp`
- Modify: `platformio.ini` (`[env:native]` `build_src_filter`)

- [ ] **Step 1: Write the failing test**

`test/test_lite_provision/test_lite_provision.cpp`:
```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../src/lite/lite_provision.h"
#include <cstring>

TEST_CASE("decide: connected always wins") {
  CHECK(lite_provision_decide(true,  true, 0, 999999, 60000) == LiteProvisionAction::StaOnline);
  CHECK(lite_provision_decide(false, true, 0, 10,     60000) == LiteProvisionAction::StaOnline);
}
TEST_CASE("decide: no creds -> AP immediately") {
  CHECK(lite_provision_decide(false, false, 0, 0, 60000) == LiteProvisionAction::EnterAp);
}
TEST_CASE("decide: creds + connecting within timeout -> wait") {
  CHECK(lite_provision_decide(true, false, 1000, 5000, 60000) == LiteProvisionAction::StaWait); // elapsed 4000
}
TEST_CASE("decide: creds + timeout reached -> AP (inclusive)") {
  CHECK(lite_provision_decide(true, false, 0, 60000, 60000) == LiteProvisionAction::EnterAp);
  CHECK(lite_provision_decide(true, false, 0, 59999, 60000) == LiteProvisionAction::StaWait);
}
TEST_CASE("decide: elapsed is wrap-safe") {
  uint32_t start = 0xFFFFF000u, now = 0x00000400u; // delta = 0x1400 = 5120
  CHECK(lite_provision_decide(true, false, start, now, 60000) == LiteProvisionAction::StaWait);
  CHECK(lite_provision_decide(true, false, start, now, 5000)  == LiteProvisionAction::EnterAp);
}
TEST_CASE("retry: only when creds failed, on interval, wrap-safe") {
  CHECK_FALSE(lite_provision_should_retry_sta(false, 0, 999999, 1000)); // no creds -> never
  CHECK(lite_provision_should_retry_sta(true, 0, 1000, 1000));          // exactly interval -> yes
  CHECK_FALSE(lite_provision_should_retry_sta(true, 0, 999, 1000));     // before -> no
  uint32_t s = 0xFFFFFF00u, n = 0x00000064u;                            // delta 356
  CHECK(lite_provision_should_retry_sta(true, s, n, 300));              // 356>=300
  CHECK_FALSE(lite_provision_should_retry_sta(true, s, n, 400));        // 356<400
}
TEST_CASE("ap ssid format + truncation") {
  char b[32]; lite_provision_ap_ssid("a1b2c3", b, sizeof(b));
  CHECK(std::strcmp(b, "OpenEVSE-Lite-a1b2c3") == 0);
  char s[10]; lite_provision_ap_ssid("a1b2c3", s, sizeof(s));           // cap 10 -> 9 chars + NUL
  CHECK(std::strlen(s) == 9); CHECK(s[9] == '\0');
}
TEST_CASE("enc map: 0 open, else secured") {
  CHECK(lite_provision_enc(0) == 0);
  CHECK(lite_provision_enc(3) == 1);
  CHECK(lite_provision_enc(7) == 1);
}
TEST_CASE("url decode: percent, plus, passthrough") {
  char o[64];
  CHECK(lite_url_decode("a%20b+c", 7, o, sizeof(o)) == 5); CHECK(std::strcmp(o, "a b c") == 0);
  CHECK(lite_url_decode("%41%42", 6, o, sizeof(o)) == 2);  CHECK(std::strcmp(o, "AB") == 0);
  CHECK(lite_url_decode("ab%", 3, o, sizeof(o)) == 3);     CHECK(std::strcmp(o, "ab%") == 0); // malformed tail -> literal
  CHECK(lite_url_decode("%2", 2, o, sizeof(o)) == 2);      CHECK(std::strcmp(o, "%2") == 0);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pio test -e native -f test_lite_provision`
Expected: FAIL — `lite_provision.h` not found / undefined symbols.

- [ ] **Step 3: Write the header**

`src/lite/lite_provision.h`:
```cpp
#pragma once
#include <stdint.h>
#include <stddef.h>

// Pure WiFi provisioning decision logic (no radio, no I/O) — native-testable.

enum class LiteProvisionAction : uint8_t { StaWait = 0, StaOnline = 1, EnterAp = 2 };

// Boot/runtime decision. elapsed = (uint32_t)(now_ms - attempt_start_ms) (wrap-safe).
//  connected            -> StaOnline
//  !has_creds           -> EnterAp
//  elapsed >= timeout    -> EnterAp
//  else                 -> StaWait
LiteProvisionAction lite_provision_decide(bool has_creds, bool sta_connected,
                                          uint32_t attempt_start_ms, uint32_t now_ms,
                                          uint32_t timeout_ms);

// True when an AP-mode unit (that fell back because stored creds failed) should
// re-attempt STA. since = (uint32_t)(now_ms - ap_since_ms). Never true if !creds_failed.
bool lite_provision_should_retry_sta(bool creds_failed, uint32_t ap_since_ms,
                                     uint32_t now_ms, uint32_t interval_ms);

// Writes "OpenEVSE-Lite-<shortid>" into out, NUL-terminated, truncated to cap.
void lite_provision_ap_ssid(const char *shortid, char *out, size_t cap);

// Maps a platform auth-mode enum to the /scan enc field: 0 (open) iff auth_mode==0.
int lite_provision_enc(int auth_mode);

// URL-decodes in[0..in_len) into out (cap incl. NUL). '+' -> space, %HH -> byte,
// malformed/truncated '%' sequences pass through literally. Returns decoded length.
size_t lite_url_decode(const char *in, size_t in_len, char *out, size_t cap);
```

- [ ] **Step 4: Write the implementation**

`src/lite/lite_provision.cpp`:
```cpp
#include "lite_provision.h"
#include <string.h>

LiteProvisionAction lite_provision_decide(bool has_creds, bool sta_connected,
                                          uint32_t attempt_start_ms, uint32_t now_ms,
                                          uint32_t timeout_ms) {
  if (sta_connected) return LiteProvisionAction::StaOnline;
  if (!has_creds)    return LiteProvisionAction::EnterAp;
  uint32_t elapsed = (uint32_t)(now_ms - attempt_start_ms);
  return (elapsed >= timeout_ms) ? LiteProvisionAction::EnterAp
                                 : LiteProvisionAction::StaWait;
}

bool lite_provision_should_retry_sta(bool creds_failed, uint32_t ap_since_ms,
                                     uint32_t now_ms, uint32_t interval_ms) {
  if (!creds_failed) return false;
  return (uint32_t)(now_ms - ap_since_ms) >= interval_ms;
}

void lite_provision_ap_ssid(const char *shortid, char *out, size_t cap) {
  if (!out || cap == 0) return;
  snprintf(out, cap, "OpenEVSE-Lite-%s", shortid ? shortid : "");
}

int lite_provision_enc(int auth_mode) { return auth_mode == 0 ? 0 : 1; }

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

size_t lite_url_decode(const char *in, size_t in_len, char *out, size_t cap) {
  size_t o = 0;
  for (size_t i = 0; i < in_len && o + 1 < cap; i++) {
    char c = in[i];
    if (c == '+') { out[o++] = ' '; }
    else if (c == '%' && i + 2 < in_len) {
      int hi = hexval(in[i+1]), lo = hexval(in[i+2]);
      if (hi >= 0 && lo >= 0) { out[o++] = (char)((hi << 4) | lo); i += 2; }
      else { out[o++] = c; }            // malformed -> literal '%'
    } else { out[o++] = c; }            // includes truncated trailing '%'
  }
  if (cap > 0) out[o] = '\0';
  return o;
}
```
Note: `snprintf` truncation gives the cap-10 → 9-char test. `i + 2 < in_len` (not `<=`) makes a 2-char `"%2"` fall to literal passthrough, matching the test.

- [ ] **Step 5: Add to native build filter**

In `platformio.ini` `[env:native]` `build_src_filter`, append ` +<lite/lite_provision.cpp>` to the existing list (after `+<lite/lite_shaper.cpp>`).

- [ ] **Step 6: Run test to verify it passes**

Run: `pio test -e native -f test_lite_provision`
Expected: PASS (all cases). Also run `pio test -e native` — all suites green.

- [ ] **Step 7: Commit**

```bash
git add src/lite/lite_provision.h src/lite/lite_provision.cpp test/test_lite_provision/ platformio.ini
git commit -m "feat(lite): pure WiFi-provisioning decision unit (native-tested)"
```

---

## Task 2: Boot rework — stored-only creds + bounded connect + AP fallback

**Files:**
- Modify: `src/lite/main_lite.cpp` (setup() WiFi bring-up + loop() retry hook)
- Modify: `platformio.ini` (`[env:openevse_lite]` — drop `LITE_WIFI_SSID/PASS` + `*_DEFAULT`)

**Context:** Today `setup()` does (in order) PF11 reset, `Serial.begin`, `ESPAL.begin`, then `WiFi.begin(LITE_WIFI_SSID, LITE_WIFI_PASS)` with a blocking `while(!WL_CONNECTED) delay(250)`, then `s_backend.begin()`, `lite_config_begin()`, totals load, `web_server_lite_begin()`, then the ATmega RESET pulse. **`lite_config_begin()` must move ahead of the WiFi bring-up** (creds come from the store now). The ATmega reset pulse + `web_server_lite_begin()` ordering relative to `s_backend.begin()` must be preserved.

- [ ] **Step 1: Reorder + replace the WiFi bring-up**

Replace the build-flag macros + `WiFi.begin(...)` blocking loop. New shape in `setup()` (after `Serial.begin`/`ESPAL.begin`, BEFORE `s_backend.begin()` so config is mounted, but keep backend/web/ATmega order otherwise):
```cpp
  lite_config_begin();                 // mount KVS first — creds + config live here now

  // Try stored creds (D1 stored-only). Bounded connect; fall to AP (D2).
  LiteWifiConfig creds;
  bool haveCreds = lite_config_load_wifi(creds);
  if (haveCreds) {
    WiFi.begin(creds.ssid.c_str(), creds.pass.c_str());
    uint32_t start = millis();
    while (lite_provision_decide(true, WiFi.status() == WL_CONNECTED,
                                 start, millis(), LITE_STA_CONNECT_TIMEOUT_MS)
           == LiteProvisionAction::StaWait) {
      delay(250);
    }
  }
  bool online = (WiFi.status() == WL_CONNECTED);
  if (online) {
    web_server_lite_set_ap_mode(false);
  } else {
    // AP_PROVISION: open softAP at 192.168.4.1, SSID OpenEVSE-Lite-<shortid>.
    char ssid[32];
    lite_provision_ap_ssid(ESPAL.getShortId().c_str(), ssid, sizeof(ssid));
    WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
    WiFi.softAP(ssid);
    web_server_lite_set_ap_mode(true);
    s_apCredsFailed = haveCreds;        // D3: only retry STA if creds existed
    s_apSinceMs     = millis();
  }
```
Add near the top of `main_lite.cpp`:
```cpp
#include "lite_provision.h"
#ifndef LITE_STA_CONNECT_TIMEOUT_MS
#define LITE_STA_CONNECT_TIMEOUT_MS 60000u
#endif
#ifndef LITE_AP_RETRY_INTERVAL_MS
#define LITE_AP_RETRY_INTERVAL_MS 300000u
#endif
static bool     s_apCredsFailed = false;
static uint32_t s_apSinceMs     = 0;
```
Remove the `LITE_WIFI_SSID`/`LITE_WIFI_PASS` `#ifndef`/`#define` block entirely.

- [ ] **Step 2: Add the D3 retry hook in `loop()`**

Near the top of `loop()` (only meaningful in AP mode; `web_server_lite_in_ap_mode()` from Task 3, or track locally), add:
```cpp
  if (web_server_lite_in_ap_mode() &&
      lite_provision_should_retry_sta(s_apCredsFailed, s_apSinceMs, millis(),
                                      LITE_AP_RETRY_INTERVAL_MS)) {
    WiFi.softAPdisconnect(true);
    LiteWifiConfig c;
    if (lite_config_load_wifi(c)) {
      WiFi.begin(c.ssid.c_str(), c.pass.c_str());
      uint32_t start = millis();
      while (lite_provision_decide(true, WiFi.status() == WL_CONNECTED,
                                   start, millis(), LITE_STA_CONNECT_TIMEOUT_MS)
             == LiteProvisionAction::StaWait) { delay(250); }
    }
    if (WiFi.status() == WL_CONNECTED) {
      web_server_lite_set_ap_mode(false);
    } else {
      char ssid[32];
      lite_provision_ap_ssid(ESPAL.getShortId().c_str(), ssid, sizeof(ssid));
      WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
      WiFi.softAP(ssid);
      s_apSinceMs = millis();           // reset the retry window
    }
  }
```
(`web_server_lite_set_ap_mode` / `web_server_lite_in_ap_mode` are declared in Task 3's header change; Task 2 may compile only after Task 3's signatures exist — implement Task 3's two setters first if needed, or stub them. Reviewer: confirm link order.)

- [ ] **Step 3: Drop build-flag creds from `platformio.ini`**

In `[env:openevse_lite]` remove `-D LITE_WIFI_SSID_DEFAULT=...`, `-D LITE_WIFI_PASS_DEFAULT=...` (and any `LITE_WIFI_SSID/PASS` references). Leave a comment: `# WiFi creds are provisioned at runtime via softAP (no compile-time creds — see lite_provision).`

- [ ] **Step 4: Build**

Run: `pio run -e openevse_lite`
Expected: SUCCESS. (No WiFi creds embedded — confirm with `grep -ac 'OpenEVSE-Lite-' .pio/build/openevse_lite/firmware.bin` ≥ 1 and that no real SSID string is present.)

- [ ] **Step 5: Commit**

```bash
git add src/lite/main_lite.cpp platformio.ini
git commit -m "feat(lite): stored-only WiFi creds + softAP fallback boot (drop build-flag path)"
```

---

## Task 3: Mode-aware serving + `/scan` + `/connect`

**Files:**
- Modify: `src/lite/web_server_lite.cpp` and its header (declare the two AP-mode setters + a deferred-reboot pump)

- [ ] **Step 1: Add AP-mode state + accessors**

In `web_server_lite.cpp` near the other file-statics:
```cpp
static bool     s_apMode       = false;
static bool     s_rebootPending = false;
static uint32_t s_rebootAtMs    = 0;
```
Add (and declare in the lite web header):
```cpp
void web_server_lite_set_ap_mode(bool ap) { s_apMode = ap; }
bool web_server_lite_in_ap_mode()          { return s_apMode; }
// Call every loop() so a queued post-/connect reboot fires after the response flushes.
void web_server_lite_pump() {
  if (s_rebootPending && (int32_t)(millis() - s_rebootAtMs) >= 0) { ESPAL.reset(); }
}
```
Wire `web_server_lite_pump();` into `main_lite.cpp` `loop()`.

- [ ] **Step 2: Embed assets (placeholder for now)**

Create `src/lite/web_ui_lite.h` with placeholder gzipped bytes (real bundles arrive in Task 5):
```cpp
#pragma once
#include <stdint.h>
// Placeholder gzip of "<!doctype html><title>lite</title>ok" — swapped for the real
// GUI bundle in Task 5. Regenerate: gzip -9 -c index.html > x && xxd -i x.
static const uint8_t INDEX_HTML_GZ[] = { /* placeholder bytes */ };
static const uint8_t SETUP_HTML_GZ[] = { /* placeholder bytes */ };
static const unsigned INDEX_HTML_GZ_LEN = sizeof(INDEX_HTML_GZ);
static const unsigned SETUP_HTML_GZ_LEN = sizeof(SETUP_HTML_GZ);
```
(The implementer generates real placeholder bytes via `printf '<!doctype html>ok' | gzip -9 | xxd -i`.)

- [ ] **Step 3: Serve the bundle at `GET /` by mode**

Replace the current `GET /` plain-text branch:
```cpp
  } else if (mg_vcmp(&hm->uri, "/") == 0) {
    const uint8_t *body = s_apMode ? SETUP_HTML_GZ : INDEX_HTML_GZ;
    unsigned len        = s_apMode ? SETUP_HTML_GZ_LEN : INDEX_HTML_GZ_LEN;
    mg_send_head(nc, 200, len, "Content-Type: text/html\r\nContent-Encoding: gzip");
    mg_send(nc, body, len);
```
(`#include "web_ui_lite.h"` at the top.)

- [ ] **Step 4: Add `/scan` (ArduinoJson, glue-built)**

New branch + handler. Synchronous scan with the UAF guard from `WiFiScan.cpp`:
```cpp
static void handle_scan(struct mg_connection *nc) {
  int n = WiFi.scanNetworks();           // blocking; returns count (or <0 on error)
  StaticJsonDocument<1024> doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < n && i < 32; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;    // omit hidden
    JsonObject o = arr.createNestedObject();
    o["ssid"] = ssid;
    o["rssi"] = WiFi.RSSI(i);
    o["enc"]  = lite_provision_enc((int)WiFi.encryptionType(i));
  }
  WiFi.scanDelete();                      // safe: scanNetworks() returned (not running)
  String body; serializeJson(doc, body);
  mg_send_head(nc, 200, body.length(), "Content-Type: application/json");
  mg_printf(nc, "%s", body.c_str());
}
```
Reviewer: confirm `WiFi.SSID(int)/RSSI(int)/encryptionType(int)` signatures against the libretiny WiFiScan API; adjust the cast/return type of `encryptionType` if it's an enum.

- [ ] **Step 5: Add `/connect` (save → deferred reboot)**

```cpp
static void handle_connect(struct mg_connection *nc, struct http_message *hm) {
  char rawSsid[64], rawPass[128];
  int sl = mg_get_http_var(&hm->query_string, "ssid", rawSsid, sizeof(rawSsid));
  int pl = mg_get_http_var(&hm->query_string, "pass", rawPass, sizeof(rawPass));
  if (sl <= 0) {
    const char *e = "{\"msg\":\"ssid required\"}";
    mg_send_head(nc, 400, strlen(e), "Content-Type: application/json");
    mg_printf(nc, "%s", e); return;
  }
  LiteWifiConfig c;
  { char d[64]; lite_url_decode(rawSsid, sl, d, sizeof(d)); c.ssid = d; }
  if (pl > 0) { char d[128]; lite_url_decode(rawPass, pl, d, sizeof(d)); c.pass = d; }
  bool saved = lite_config_save_wifi(c);
  if (!saved) {
    const char *e = "{\"msg\":\"save failed\"}";
    mg_send_head(nc, 500, strlen(e), "Content-Type: application/json");
    mg_printf(nc, "%s", e); return;
  }
  const char *ok = "{\"msg\":\"OK\"}";
  mg_send_head(nc, 200, strlen(ok), "Content-Type: application/json");
  mg_printf(nc, "%s", ok);
  s_rebootPending = true; s_rebootAtMs = millis() + 750;   // reboot after flush
}
```
Note: `mg_get_http_var` already URL-decodes once; the extra `lite_url_decode` is belt-and-suspenders for `+`/`%` the GUI double-encodes. Reviewer: verify we're not double-decoding legitimately-decoded values — if `mg_get_http_var` fully decodes, drop the second decode and store `rawSsid`/`rawPass` directly. (Pick one decode path; the pure `lite_url_decode` test stands regardless.)

- [ ] **Step 6: Route the two new endpoints**

In the URI dispatch chain add before the `GET /` branch:
```cpp
  } else if (mg_vcmp(&hm->uri, "/scan") == 0) {
    handle_scan(nc);
  } else if (mg_vcmp(&hm->uri, "/connect") == 0) {
    handle_connect(nc, hm);
```

- [ ] **Step 7: Build**

Run: `pio run -e openevse_lite`
Expected: SUCCESS.

- [ ] **Step 8: Commit**

```bash
git add src/lite/web_server_lite.cpp src/lite/web_server_lite.h src/lite/web_ui_lite.h src/lite/main_lite.cpp
git commit -m "feat(lite): mode-aware GET / serving + /scan + /connect provisioning endpoints"
```

---

## Task 4: Decode-path reconciliation + native + device regression

**Files:**
- Modify: `src/lite/web_server_lite.cpp` (finalize single decode path per Task 3 Step 5 note)

- [ ] **Step 1: Verify the decode path**

Manually confirm whether `mg_get_http_var` (MongooseLite 6.x) URL-decodes its output. Grep the vendored mongoose source. If it decodes, remove the second `lite_url_decode` in `handle_connect` and assign `rawSsid`/`rawPass` directly (keeping `lite_url_decode` available for any path mongoose does not cover). Document the chosen path in a one-line comment.

- [ ] **Step 2: Run the full native suite**

Run: `pio test -e native`
Expected: all suites PASS (22 incl. `test_lite_provision`).

- [ ] **Step 3: Device build + size check**

Run: `pio run -e openevse_lite`
Expected: SUCCESS. Record `firmware.bin` size; `python3 -c "print(<size>/983040)"` — note headroom (placeholder bundle is tiny; real check is Task 5).

- [ ] **Step 4: Commit**

```bash
git add src/lite/web_server_lite.cpp
git commit -m "fix(lite): single URL-decode path for /connect params"
```

---

## Task 5: Integrate real GUI bundles + budget gate + HW validation

**Files:**
- Modify: `src/lite/web_ui_lite.h` (real gzipped bytes)

**Depends on:** the UI agent delivering `dist/index.html.gz` + `dist/setup.html.gz` (sync-point a). Until then, this task is BLOCKED — leave the placeholder and mark Task 5 pending.

- [ ] **Step 1: Embed the real bundles**

From the GUI `dist/`:
```bash
xxd -i -n INDEX_HTML_GZ dist/index.html.gz  > /tmp/idx.h
xxd -i -n SETUP_HTML_GZ dist/setup.html.gz  > /tmp/setup.h
```
Splice both arrays into `src/lite/web_ui_lite.h`, keeping the `*_LEN = sizeof(...)` lines.

- [ ] **Step 2: Build + budget gate**

Run: `pio run -e openevse_lite`
Expected: SUCCESS and `firmware.bin / 983040 < 0.85` (≈ < 835 KB). If over, the bundle must shrink (GUI side).

- [ ] **Step 3: HW validation (user-assisted)**

Flash (creds-free build). On the bench:
1. Unit boots into AP `OpenEVSE-Lite-<id>` (open). **User joins it.**
2. Browse `http://192.168.4.1/` → setup page renders.
3. `GET /scan` lists real APs (`ssid/rssi/enc`).
4. Submit home WiFi via `/connect` → `{"msg":"OK"}` → unit reboots.
5. Unit comes up in STA, serves the runtime app at its DHCP IP; `/status` live.
6. Reflash → confirm creds persist (KVS survives OTA): still STA, no AP.

Record results in the `lite-wifi-provisioning` memory. Charge-control remains independently GFI-blocked (unchanged).

- [ ] **Step 4: Commit**

```bash
git add src/lite/web_ui_lite.h
git commit -m "feat(lite): embed openevse-gui-lite bundles (runtime + setup)"
```

---

## Final review

After Task 4 (Task 5 gated on the GUI bundle), dispatch a final holistic code reviewer over the whole provisioning change: boot state-machine correctness (no unbounded blocking, AP fallback, retry window reset), serving correctness (gzip headers, mode flag), `/connect` save+reboot ordering (response flushes before reset), and that no real creds exist in the binary. Then `superpowers:finishing-a-development-branch`.
