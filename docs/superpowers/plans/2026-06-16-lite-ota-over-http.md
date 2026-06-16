# Lite OTA-over-HTTP Implementation Plan

> **For agentic workers:** Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add firmware OTA to the lite app — `POST /update` accepts a dual-bank `.uf2`, streams it into the inactive bank via the Arduino `Update` API, reboots, and the new image self-confirms once healthy so it sticks (no rollback).

**Architecture:** Standard synchronous-`WebServer` file-upload handler feeding the LibreTiny `Update` API (fork branch `feature/silabs-efm32gg11-ota`, bench-verified 2026-06-16). A pure decision helper (`lite_ota_should_confirm`) decides when `loop()` calls `lt_ota_confirm()`. Reuses the existing deferred-reboot mechanism in `web_server_lite.cpp`.

**Tech Stack:** C++ / LibreTiny / EFM32GG11 (WGM160P), ESP-style `WebServer`, `lt_ota_*` (uf2ota), doctest (native env).

> **Implementation deviations from this plan (as shipped, HW-validated 2026-06-16):**
> - Receive path drives the low-level `lt_ota_begin/write/end` directly, **not** Arduino `Update` — `Update.cpp` pulls an MD5 impl silabs-efm32gg11 disables (`LT_ARD_MD5_MBEDTLS 0`), and we never used its optional MD5. Success gate hardened to `lt_ota_end() && error==UF2_ERR_OK && bytes_written>0` (an empty/garbage POST otherwise falsely succeeded + rebooted).
> - `lt_ota_confirm()` is called **unconditionally, early in `setup()` before WF200 bring-up** — it is idempotent (libretiny ed495b5: no-op unless the running bank is a genuine unconfirmed TRIAL). The Task 1 `lite_ota_should_confirm` helper + its test were **removed** (the subsystem owns the gating; calling confirm in `loop()` against erased metadata had wedged the WF200 link).
> - Build env: `platform` repinned to `symlink:///home/rar/dev/libretiny` (OTA branch HEAD) for the real `_efm32_uf2` UF2 builder; real `.uf2` packed with `PYTHONPATH=/home/rar/dev/ltchiptool` (EFM32GG11 plugin, ltchiptool#93).

**Decisions (user-approved 2026-06-16):**
- Confirm gate = **firmware-liveness only** (WiFi associated + uptime threshold). NOT gated on EVSE/backend health — the bench is permanently in GFI fault, which would otherwise roll back every OTA.
- `/update` is **unauthenticated**, consistent with every other lite endpoint.

---

### Task 1: Pure confirm-gate helper (`lite_ota`)

**Files:**
- Create: `src/lite/lite_ota.h`, `src/lite/lite_ota.cpp`
- Test: `test/test_lite_ota/test_lite_ota.cpp`
- Modify: `platformio.ini` ([env:native] `build_src_filter` += `+<lite/lite_ota.cpp>`)

- [ ] **Step 1: Write the failing test** (`test/test_lite_ota/test_lite_ota.cpp`)

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../src/lite/lite_ota.h"

TEST_CASE("confirm only once all conditions hold") {
  CHECK(lite_ota_should_confirm(false, true, 10000, 10000));   // exactly threshold
  CHECK(lite_ota_should_confirm(false, true, 999999, 10000));
}
TEST_CASE("not before threshold") {
  CHECK_FALSE(lite_ota_should_confirm(false, true, 9999, 10000));
}
TEST_CASE("not while disconnected") {
  CHECK_FALSE(lite_ota_should_confirm(false, false, 999999, 10000));
}
TEST_CASE("not if already confirmed") {
  CHECK_FALSE(lite_ota_should_confirm(true, true, 999999, 10000));
}
```

- [ ] **Step 2: Run test, verify it fails** — `pio test -e native -f test_lite_ota` → FAIL (no `lite_ota.h`)

- [ ] **Step 3: Implement** (`lite_ota.h` + `lite_ota.cpp`)

```cpp
// lite_ota.h
#pragma once
#include <stdint.h>
// Pure: should the running image call lt_ota_confirm() now? True exactly when not
// yet confirmed, WiFi is associated, and uptime has reached the threshold. Gated on
// firmware liveness ONLY (never EVSE/backend health) so a faulted EVSE can't force
// perpetual OTA rollback.
bool lite_ota_should_confirm(bool already_confirmed, bool wifi_connected,
                             uint32_t uptime_ms, uint32_t threshold_ms);
```

```cpp
// lite_ota.cpp
#include "lite_ota.h"
bool lite_ota_should_confirm(bool already_confirmed, bool wifi_connected,
                             uint32_t uptime_ms, uint32_t threshold_ms) {
  return !already_confirmed && wifi_connected && uptime_ms >= threshold_ms;
}
```

- [ ] **Step 4: Add to native build_src_filter, run test** — append `+<lite/lite_ota.cpp>`; `pio test -e native -f test_lite_ota` → PASS

- [ ] **Step 5: Commit** (only when the user asks — see standing constraint)

### Task 2: `POST /update` route + `ota_bank` in `/status`

**Files:**
- Modify: `src/lite/web_server_lite.cpp`

- [ ] **Step 1: Include + OTA state** — add `#include <Update.h>` and `#include "api/lt_ota.h"` (for `lt_ota_dual_get_current`); static `bool s_otaOk; String s_otaErr;`

- [ ] **Step 2: Upload handler** (`handle_update_upload`)

```cpp
static void handle_update_upload() {
  HTTPUpload &up = s_server.upload();
  if (up.status == UPLOAD_FILE_START) {
    s_otaErr = ""; s_otaOk = false;
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) s_otaErr = Update.errorString();
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (s_otaErr.length() == 0 &&
        Update.write(up.buf, up.currentSize) != up.currentSize) s_otaErr = Update.errorString();
  } else if (up.status == UPLOAD_FILE_END) {
    if (s_otaErr.length() == 0 && Update.end(true)) s_otaOk = true;
    else if (s_otaErr.length() == 0) s_otaErr = Update.errorString();
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.abort(); s_otaErr = "aborted";
  }
}
```

- [ ] **Step 3: Completion handler** (`handle_update_done`) — JSON result; schedule deferred reboot on success via existing `s_rebootPending`/`s_rebootAtMs`

```cpp
static void handle_update_done() {
  StaticJsonDocument<128> doc;
  doc["ok"] = s_otaOk;
  if (!s_otaOk) doc["error"] = s_otaErr.length() ? s_otaErr : String("unknown");
  String out; serializeJson(doc, out);
  s_server.send(s_otaOk ? 200 : 400, "application/json", out);
  if (s_otaOk) { s_rebootPending = true; s_rebootAtMs = millis() + 750; }
}
```

- [ ] **Step 4: Register route** (in `web_server_lite_begin`, beside the others)

```cpp
s_server.on("/update", HTTP_POST, handle_update_done, handle_update_upload);
```

- [ ] **Step 5: `ota_bank` in `/status`** — beside `firmware`/`version`: `doc["ota_bank"] = lt_ota_dual_get_current();`

- [ ] **Step 6: Build** — `pio run -e openevse_lite` → SUCCESS

- [ ] **Step 7: Commit** (only when the user asks)

### Task 3: Confirm hook in `main_lite` loop

**Files:**
- Modify: `src/lite/main_lite.cpp`

- [ ] **Step 1: Include + state** — `#include "lite_ota.h"`; `static bool s_otaConfirmed = false;`. `lt_ota_confirm()` is declared via `<Arduino.h>`; add `#include "api/lt_ota.h"` if it doesn't resolve.

- [ ] **Step 2: Wire in `loop()`** (early, before the EVSE glue)

```cpp
if (lite_ota_should_confirm(s_otaConfirmed, WiFi.status() == WL_CONNECTED, millis(), 10000)) {
  lt_ota_confirm();        // mark the running bank healthy so an OTA'd TRIAL sticks
  s_otaConfirmed = true;
}
```

- [ ] **Step 3: Build** — `pio run -e openevse_lite` → SUCCESS

- [ ] **Step 4: Commit** (only when the user asks)

### Task 4: Full verify + real `.uf2` + HW OTA round-trip

**Files:** none (verification)

- [ ] **Step 1: Native suite** — `pio test -e native` → all pass (incl. `test_lite_ota`)
- [ ] **Step 2: Bump version for visible swap** — in `web_server_lite.cpp`, `LITE_FW_VERSION` `"lite-web1"` → `"lite-ota1"` (so `/status.version` changes across the OTA)
- [ ] **Step 3: Build real dual-bank UF2** — `PYTHONPATH=/home/rar/dev/ltchiptool pio run -e openevse_lite`; verify `ltchiptool uf2 info .pio/build/openevse_lite/firmware.uf2` → Family EFM32GG11, ota1/ota2, `OTA_PART_LIST: 011000`, ~2× `firmware.bin`
- [ ] **Step 4: HW OTA round-trip on bench (10.75.1.216)**
  - Record pre: `GET /status` → note `version` (`lite-web1`), `ota_bank`
  - `curl -F "firmware=@.pio/build/openevse_lite/firmware.uf2" http://10.75.1.216/update` → `{"ok":true}` (200)
  - Wait for reboot + rejoin; `GET /status` → `version` == `lite-ota1`, `ota_bank` flipped, UI still served
  - **Reset the device** (power-cycle or reset); `GET /status` again → still `lite-ota1` on the flipped bank ⇒ `lt_ota_confirm()` worked, no rollback
- [ ] **Step 5: Report results** to user

---

## Self-Review notes
- Type consistency: `lite_ota_should_confirm(bool,bool,uint32_t,uint32_t)` identical across helper, test, and the `main_lite` call site.
- The `Update.errorString()` return type is `const char*`; assigning to `String s_otaErr` is fine.
- `UPDATE_SIZE_UNKNOWN` is valid for `Update.begin` per the fork usage doc (UF2 carries its own sizing).
- No silent caps: upload errors are surfaced in the JSON `error` field; no reboot on failure.
