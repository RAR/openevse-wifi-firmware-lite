# JuiceBox-lite WiFi Provisioning + UI Serving — Design Spec

**Date:** 2026-06-14
**Repo:** `~/oevse/openevse-juicebox-lite`, branch `feature/juicebox-lite`
**Status:** Approved decisions locked; ready for implementation planning.
**Companion:** the GUI side lives in `~/openevse-gui-lite`
(`docs/superpowers/specs/2026-06-14-openevse-gui-lite-design.md`). This spec is the
**authoritative firmware contract** and reconciles that spec's §7 sync-point (b):
the `/scan` + `/connect` shapes below are final, not GUI-proposed.

## Goal

Make the lite firmware provision its own WiFi over a softAP setup flow, and serve
the two GUI bundles (runtime app + AP setup page). Today the firmware connects
with **compile-time** creds (`WiFi.begin(LITE_WIFI_SSID, …)` in `main_lite.cpp`)
and serves no static files (`GET /` returns the plain text `openevse-lite`). This
slice replaces both.

## Locked decisions (user, 2026-06-14)

- **D1 — Cred source: stored-only.** Remove the `LITE_WIFI_SSID` / `LITE_WIFI_PASS`
  compile-time path entirely (and the `*_DEFAULT` placeholders in `platformio.ini`).
  Creds live solely in the FlashDB/LittleFS store (`lite_config_load_wifi` /
  `lite_config_save_wifi`, already built). A freshly-flashed unit boots into AP
  setup. **No creds ever live in the binary** (satisfies the standing
  never-commit-creds rule). The KVS partition survives OTA, so a unit is
  provisioned **once per physical device**, and creds persist across reflashes.
- **D2 — AP entry: no creds OR connect-fail.** Boot tries stored creds; enters AP
  mode if there are none, or if a STA connect doesn't succeed within a timeout.
- **D3 — Auto-recovery (mitigates D2's downside).** While in AP mode *because
  stored creds failed*, periodically drop the AP and retry STA, so a unit knocked
  off by a flaky router rejoins on its own. A unit with **no** stored creds stays
  in AP indefinitely (nothing to retry).
- **D4 — mDNS: out of scope for v1.** No `openevse-lite.local`; the setup-success
  copy uses the IP only. (The GUI spec's `.local` promise is dropped — relayed to
  the UI agent.) Revisit later if wanted.
- **D5 — Captive-portal DNS: out of scope for v1** (YAGNI; user types the IP).

## Architecture

A small **pure decision unit** drives the boot/runtime WiFi state machine; thin
device glue does the radio + HTTP. Mirrors the established lite pattern
(pure native-tested core + `web_server_lite.cpp` glue).

### Boot / runtime state machine (`lite_provision` pure unit + glue)

States: `STA_CONNECTING`, `STA_ONLINE`, `AP_PROVISION`. The pure unit decides
transitions from inputs (has-stored-creds, sta-connected, elapsed-since-attempt,
creds-failed-flag); the glue performs the radio actions.

```
boot:
  lite_config_begin()                     # mount KVS FIRST (before WiFi)
  if lite_config_load_wifi(creds):
      WiFi.begin(creds.ssid, creds.pass)
      wait up to STA_CONNECT_TIMEOUT_MS (≈60_000) for WL_CONNECTED
      connected -> STA_ONLINE
      else      -> AP_PROVISION (creds_failed = true)
  else:
      -> AP_PROVISION (creds_failed = false)

AP_PROVISION:
  WiFi.softAPConfig(192.168.4.1, 192.168.4.1, 255.255.255.0)
  WiFi.softAP("OpenEVSE-Lite-<shortid>")      # open AP, shortid = ESPAL.getShortId()
  serve setup page + /scan + /connect
  if creds_failed:                            # D3 auto-recovery
      every AP_RETRY_INTERVAL_MS (≈300_000): softAPdisconnect(); retry STA;
        on success -> STA_ONLINE; on fail -> back to AP
```

**Reorder note for the implementer:** `main_lite.cpp` setup() currently does
`WiFi.begin(...)` (blocking `while(!connected) delay(250)`) *before*
`lite_config_begin()`. Config mount must move ahead of WiFi, and the unbounded
blocking loop must become the bounded, AP-falling state machine above. The
ATmega RESET/PF11 sequence and `web_server_lite_begin()` ordering must be
preserved otherwise.

### Mode-aware serving (`web_server_lite.cpp`)

A file-static `s_apMode` flag (set by the boot glue) selects what `GET /` returns:

- **STA mode:** `GET /` → `INDEX_HTML_GZ` (`Content-Type: text/html`,
  `Content-Encoding: gzip`). Replaces the current plain-text stub.
- **AP mode:** `GET /` → `SETUP_HTML_GZ` (same headers).
- Existing API routes (`/status`, `/config`, `/override`, `/schedule`, `/ws`)
  stay registered in both modes (harmless in AP; the setup bundle just won't call
  them). `/scan` + `/connect` are added (below). Keep `MG_F_SEND_AND_CLOSE` and
  the Mongoose-6.x `mg_send_head` + `mg_printf` idiom.

## HTTP contract — provisioning endpoints (FINAL)

### `GET /scan`
Triggers a WiFi scan and returns the visible APs as a JSON array:
```json
[ { "ssid": "Hamkins-IOT", "rssi": -41, "enc": 1 }, ... ]
```
- `enc`: `0` = open, `1` = secured (any encrypted auth mode collapses to 1).
- **Firmware returns the raw scan list** (may contain duplicate SSIDs across
  channels/bands). **The GUI dedupes by SSID (keeping strongest RSSI) and sorts**
  — per the GUI spec §4. Firmware does not dedupe.
- Hidden APs (empty SSID) are omitted.
- Empty result → `200 []` (GUI shows "no networks found, retry").
- Scan is synchronous-with-guard: never call `scanDelete()` while a scan is
  running (LibreTiny UAF — see `WiFiScan.cpp` header note); poll `scanComplete()`.

### `GET /connect?ssid=<urlenc>&pass=<urlenc>`
- URL-decode both params. `pass` may be **absent** (open network) → store empty.
- Validate `ssid` non-empty → else `400 {"msg":"ssid required"}`.
- `lite_config_save_wifi({ssid, pass})`; on store failure → `500 {"msg":"save failed"}`.
- On success: send `200 {"msg":"OK"}`, **then** schedule a reboot
  (`ESPAL.reset()`) after a short delay so the response flushes (e.g. set a
  "reboot at millis()+750ms" flag checked in the loop; do NOT reset inside the
  HTTP handler before the socket drains).
- GET-param form (consistent with `/config`; avoids the no-Content-Length POST
  body-wedge in Mongoose).

### AP descriptor (for the GUI's success copy + docs)
- **AP SSID:** `OpenEVSE-Lite-<shortid>` (`shortid` = 6 hex from
  `ESPAL.getShortId()`), **open** (no passphrase) for easy join.
- **Gateway / device IP in AP mode:** `192.168.4.1`.
- Success message (GUI): "Saved. The device is restarting — rejoin your home
  WiFi, then open **http://&lt;new-IP&gt;**." (No `.local`; D4.)

## Pure unit: `src/lite/lite_provision.{h,cpp}` (native-tested, doctest)

`#pragma once` + `<stdint.h>` only; no `OPENEVSE_LITE` guard; `.cpp` added to
`[env:native]` `build_src_filter`; tests in `test/test_lite_provision/`.

Pure, testable surface (no radio, no I/O):
1. **Boot decision** — `LiteProvisionAction lite_provision_decide(bool has_creds,
   bool sta_connected, uint32_t elapsed_ms, uint32_t timeout_ms)` →
   `{StaWait, StaOnline, EnterAp}`. Covers: connected→StaOnline; has_creds &&
   !connected && elapsed<timeout → StaWait; has_creds && !connected &&
   elapsed≥timeout → EnterAp; !has_creds → EnterAp.
2. **AP-retry gate** — `bool lite_provision_should_retry_sta(bool creds_failed,
   uint32_t since_ap_ms, uint32_t interval_ms)` (wrap-safe; false when
   !creds_failed).
3. **AP SSID builder** — `void lite_provision_ap_ssid(const char *shortid,
   char *out, size_t cap)` → `"OpenEVSE-Lite-<shortid>"`.
4. **`enc` mapper** — `int lite_provision_enc(int auth_mode)` → 0 for open, 1
   otherwise (maps the LibreTiny/WF200 auth enum).
5. **scan response** — built in the **glue** with ArduinoJson (like every other
   lite response), not in the pure unit: this gets correct JSON-escaping of
   arbitrary SSIDs for free and matches `build_status_json`. The glue iterates
   `WiFi.SSID(i)/RSSI(i)/encryptionType(i)`, maps enc via `lite_provision_enc`,
   and omits empty SSIDs. (Refinement from an earlier "pure serializer" idea —
   hand-rolling JSON escaping is the fragile path.)

URL-decode for `/connect`: reuse an existing helper if one exists in the lite
tree; otherwise add `lite_url_decode` here (pure, tested: `%20`→space, `+`→space,
malformed `%` passthrough).

## Flash budget (the gating constraint)

Lite is dual-OTA: real ceiling = **960 KB OTA slot (983040 B)**. Current firmware
~519 KB → ~464 KB headroom. Both gzipped bundles + scan/connect/AP code must fit.
**Acceptance gate:** after embedding the real bundles, `pio run -e openevse_lite`
and confirm `firmware.bin / 983040 < ~85%`. Until the GUI delivers real bundles,
build against a tiny placeholder `.gz` (sync-point (a)).

## Testing

- **Native (doctest):** the full `lite_provision` surface above — boot decision
  table, retry gate (incl. wrap), AP SSID format, enc mapping, scan JSON
  (incl. empty-SSID omission + empty list), URL-decode. Keep the native suite
  green (currently 21 suites).
- **Device build:** `pio run -e openevse_lite` green at each glue task.
- **HW validation (partly deferred / needs user):** the AP-join + `/connect`
  round-trip requires a human to join the open `OpenEVSE-Lite-<id>` AP and submit
  the form (the lab network can't reach 192.168.4.1). Checkable by the user:
  AP appears, setup page served, `/scan` lists real APs, `/connect` saves +
  reboots, unit comes up in STA on the chosen network, creds persist across a
  reflash. The STA-online serving of the runtime bundle is checkable once any
  unit is provisioned. Charge-control HW validation remains independently blocked
  on the GFI-faulted bench (unchanged by this slice).

## Task decomposition (for the plan)

1. **T1** — `lite_provision` pure unit (decision + retry + ssid + enc + scan JSON
   + url-decode), native doctest. *(pure, TDD)*
2. **T2** — `main_lite.cpp` boot rework: mount config first; bounded STA connect
   with timeout; AP fallback; D3 periodic retry; remove the build-flag cred path
   + `platformio.ini` `*_DEFAULT` placeholders. *(device build)*
3. **T3** — `web_server_lite.cpp`: `s_apMode` flag + mode-aware `GET /` serving
   (gzipped asset, placeholder header for now) + `/scan` handler + `/connect`
   handler (save → deferred reboot). *(device build)*
4. **T4** — Asset embedding mechanism: generated header(s) `web_ui_lite.h` /
   `web_setup_lite.h` holding the gzipped bundle bytes + length, plus the route
   wiring to serve them. Placeholder bytes until the GUI bundle lands. *(device build)*
5. **T5** — Integrate real GUI bundles (swap placeholders), budget gate, build +
   the user-driven HW validation checklist above. *(integration + HW)*

## Cross-agent sync points

- **(a)** Firmware can build serving against a placeholder `.gz` immediately; swap
  in `dist/index.html.gz` + `dist/setup.html.gz` when the GUI agent delivers.
- **(b)** `/scan` + `/connect` shapes here are **final** — the GUI codes to these.
- **(c)** mDNS dropped (D4): GUI removes the `.local` line from setup-success copy.
- **(d)** Final budget check is firmware-side after embedding real bundles.
