# OpenEVSE Lite — Slice 1 (JuiceBox 40 / LibreTiny) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Boot the OpenEVSE WiFi-companion firmware on the Enel X JuiceBox 40 (Silicon Labs WGM160P / EFM32GG11 + WF200) via LibreTiny, connect WiFi (STA), and serve `/status` over plain HTTP backed by the in-firmware FakeEVSE stub.

**Architecture:** A new `openevse_lite` PlatformIO env (`platform = libretiny`, `board = wgm160p-juicebox-40`, `framework = arduino`) compiled behind a `-D OPENEVSE_LITE` umbrella gate that strips everything heavy. The networking spine is ArduinoMongoose driven over LibreTiny lwIP via `MG_NET_IF_SOCKET` (the same BSD-socket path ESP32 uses) — enabled by a `CS_P_CUSTOM` Mongoose platform header plus `LWIP_SOCKET` in the LibreTiny fork's `lwipopts.h`. Chip services normally provided by ESPAL/ESP-IDF are supplied by a thin self-contained `src/lite/` shim.

**Tech Stack:** LibreTiny (silabs-efm32gg11 family, PR #387), Arduino core, ArduinoMongoose (Mongoose 6), lwIP sockets, LittleFS, EFM32 TRNG0, doctest (native host tests).

**Spec:** `docs/superpowers/specs/2026-06-12-juicebox-lite-port-design.md`

---

## Reality of this slice (read before starting)

- The **LibreTiny EFM32 platform is in the author's unmerged fork** (libretiny-eu/libretiny PR #387) and the **only WGM160P board is the author's JuiceBox 40**. Therefore: the implementer writes code and runs **native doctests on the dev host**; the **author builds, flashes, and validates on hardware** at each HW checkpoint. Do not claim a HW step passed — present the build artifact/diff and the exact command + expected serial/curl output for the author to run.
- `platformio.ini` must pin the LibreTiny platform to the author's fork/branch carrying PR #387. The exact URL is author-supplied at Task 1; the plan references it as `${LIBRETINY_FORK}` and the implementer must ask for it before writing the env (a NEEDS_CONTEXT, not a guess).
- WiFi is **STA-only with pre-provisioned credentials** this slice. SSID/PSK come from a build flag (`-D LITE_WIFI_SSID`/`-D LITE_WIFI_PASS`) for the spike and from persisted config thereafter. AP/softAP provisioning is out of scope (rides on the author's in-progress softAP work).

## File structure

| File | Responsibility | Task |
|------|----------------|------|
| `platformio.ini` (`[env:openevse_lite]`) | New build env, LITE gate, LibreTiny platform/board, STA creds, `build_src_filter` | 1, 5 |
| `.pio/.../ArduinoMongoose/src/mongoose.h` *(via repo patch)* — see Task 1 | `CS_P_CUSTOM` platform block for arm-none-eabi + lwIP sockets | 1 |
| `src/lite/mg_platform_lite.h` | Vendored `CS_P_CUSTOM` definitions included ahead of Mongoose | 1 |
| `src/lite/spike_main.cpp` | Task-1-only spike entry: WiFi STA + one `:80` "hello" listener | 1 (removed in Task 6) |
| `src/lite/espal_lite.h` / `.cpp` | Thin EFM32/LibreTiny backing for the ESPAL call surface | 2 |
| `src/lite/lite_random.h` / `.cpp` | TRNG0-backed `lite_random_bytes()` + Mongoose RNG symbol | 3 |
| `src/lite/lite_config_store.cpp` | LittleFS-backed config load/save; flash-erase shim | 4 |
| `src/lite/web_server_lite.cpp` / `.h` | Minimal Mongoose server + `/status` route against FakeEVSE | 6 |
| `src/lite/main_lite.cpp` | The real lite boot path (config → WiFi → server → loop) | 6 |
| `test/test_espal_lite/` | doctest: short/long-ID hex formatting, flash-size reporting | 2 |
| `test/test_lite_random/` | doctest: RNG seam contract (length, non-throwing, stub-injectable) | 3 |

`src/lite/` is a new directory so the lite target's bring-up code is isolated from the ESP32 `src/*.cpp` and can be selected with `build_src_filter` without disturbing other envs.

---

### Task 1: Networking spike — prove Mongoose-on-lwIP (Route 1)

**This is the make-or-break and must come first. No OpenEVSE code.**

**Files:**
- Create: `src/lite/mg_platform_lite.h`
- Create: `src/lite/spike_main.cpp`
- Modify: `platformio.ini` (add `[env:openevse_lite]`)
- Author-side: `lwipopts.h` in the LibreTiny fork (enable sockets)

- [ ] **Step 1: Get the LibreTiny fork URL/branch and board string from the author**

This is a NEEDS_CONTEXT stop, not a guess. Ask the author for: (a) the `platform = ` URL or `platform_packages` ref for the fork carrying PR #387, (b) the exact `board = ` id (`wgm160p-juicebox-40` per the PR, confirm), (c) the path to the board's `lwipopts.h` in the fork.

- [ ] **Step 2: Enable lwIP sockets in the LibreTiny fork's lwipopts.h**

In the fork's `lwipopts.h` for the efm32gg11/WGM160P target, ensure:

```c
#define LWIP_SOCKET            1
#define LWIP_NETCONN           1
#define LWIP_DNS               1
#define LWIP_SO_RCVTIMEO       1
#define LWIP_SO_SNDTIMEO       1
#define LWIP_TCP_KEEPALIVE     1
#define MEMP_NUM_NETCONN       8   /* listener + a few client conns */
#define LWIP_COMPAT_SOCKETS    1   /* plain socket()/select() names */
#define LWIP_POSIX_SOCKETS_IO_NAMES 1
```

Expected: the fork still builds its `framework=base` blink/Serial example (regression check the PR already passes).

- [ ] **Step 3: Write the Mongoose custom-platform header**

`src/lite/mg_platform_lite.h` — defines the `CS_P_CUSTOM` platform so Mongoose uses BSD sockets over lwIP on arm-none-eabi:

```c
#ifndef MG_PLATFORM_LITE_H
#define MG_PLATFORM_LITE_H
// Mongoose CS_P_CUSTOM platform for LibreTiny/EFM32 (arm-none-eabi + lwIP sockets).
// Mirrors what CS_P_ESP32 sets: MG_LWIP for compat shims, but the actual I/O is
// MG_NET_IF_SOCKET over lwIP's socket layer (LWIP_SOCKET=1 in lwipopts.h).
#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <lwip/sockets.h>
#include <lwip/netdb.h>

#define SIZE_T_FMT "u"
typedef struct stat cs_stat_t;
#define DIRSEP '/'
#define to64(x) strtoll(x, NULL, 10)
#define INT64_FMT  PRId64
#define INT64_X_FMT PRIx64
#define __cdecl

#define MG_LWIP 1
#define MG_NET_IF MG_NET_IF_SOCKET
#define MG_ENABLE_FILESYSTEM 0
#define MG_ENABLE_DIRECTORY_LISTING 0
#ifndef CS_ENABLE_STDIO
#define CS_ENABLE_STDIO 1
#endif
#endif // MG_PLATFORM_LITE_H
```

Mongoose is told to use this via build flags (Step 4): `-DCS_PLATFORM=CS_P_CUSTOM` plus force-including this header so the symbols above are defined before `mongoose.h` is parsed.

- [ ] **Step 4: Add the `openevse_lite` env (spike configuration)**

Append to `platformio.ini` (use the real fork URL from Step 1 in place of `${LIBRETINY_FORK}`):

```ini
[env:openevse_lite]
platform = ${LIBRETINY_FORK}
board = wgm160p-juicebox-40
framework = arduino
# Slice-1 spike: compile ONLY the networking spike + Mongoose. No OpenEVSE src yet.
build_src_filter = -<*> +<lite/spike_main.cpp>
build_flags =
  -D OPENEVSE_LITE
  -D JUICEBOX_40
  -D MG_ENABLE_SSL=0
  -D CS_PLATFORM=CS_P_CUSTOM
  -include src/lite/mg_platform_lite.h
  -D LITE_WIFI_SSID=\"CHANGEME\"
  -D LITE_WIFI_PASS=\"CHANGEME\"
lib_deps =
  https://github.com/RAR/ArduinoMongoose.git#cf237c4b225a5ed11a836aa53047ede1d5509336
monitor_speed = 115200
```

- [ ] **Step 5: Write the spike entry**

`src/lite/spike_main.cpp`:

```cpp
#include <Arduino.h>
#include <WiFi.h>          // LibreTiny Arduino WiFi
#include "mongoose.h"

static struct mg_mgr s_mgr;

static void ev_handler(struct mg_connection *nc, int ev, void *p) {
  if (ev == MG_EV_HTTP_REQUEST) {
    mg_send_head(nc, 200, 5, "Content-Type: text/plain");
    mg_printf(nc, "hello");
    nc->flags |= MG_F_SEND_AND_CLOSE;
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("\n[lite-spike] boot, free heap reported by core\n");

  WiFi.begin(LITE_WIFI_SSID, LITE_WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) { delay(250); Serial.print('.'); }
  Serial.printf("\n[lite-spike] WiFi up, IP=%s\n", WiFi.localIP().toString().c_str());

  mg_mgr_init(&s_mgr, NULL);
  struct mg_connection *c = mg_bind(&s_mgr, "80", ev_handler);
  if (!c) { Serial.println("[lite-spike] mg_bind FAILED"); return; }
  mg_set_protocol_http_websocket(c);
  Serial.println("[lite-spike] HTTP listening on :80");
}

void loop() {
  mg_mgr_poll(&s_mgr, 100);
}
```

- [ ] **Step 6: HW checkpoint (author runs)**

Author sets real SSID/PASS, builds + flashes:

```
pio run -e openevse_lite -t upload && pio device monitor -e openevse_lite
```

Expected serial: `WiFi up, IP=…` then `HTTP listening on :80`. Then from a host on the LAN:

```
curl http://<ip>/
```

Expected: `hello`.

**Decision gate:** if `curl` returns `hello`, Route 1 is proven — proceed. If `mg_bind` fails or sockets don't link, STOP and escalate: pivot the platform header to `MG_NET_IF_LWIP_LOW_LEVEL` (Route 2 in the spec) before any further task.

- [ ] **Step 7: Commit**

```bash
git add platformio.ini src/lite/mg_platform_lite.h src/lite/spike_main.cpp
git commit -m "lite: networking spike — Mongoose over LibreTiny lwIP sockets (:80 hello)"
```

---

### Task 2: ESPAL EFM32 shim

**Files:**
- Create: `src/lite/espal_lite.h`, `src/lite/espal_lite.cpp`
- Create: `test/test_espal_lite/test_espal_lite.cpp`
- Modify: `platformio.ini` (`[env:native]` `build_src_filter`)

The firmware calls exactly these ESPAL methods: `getFreeHeap`, `getShortId`, `getLongId`, `reset`, `getFlashChipSize`, `getChipInfo`, `eraseConfig`, `begin`. `getShortId()`/`getLongId()` return `String`; `getFreeHeap()` returns a heap byte count. The shim provides a drop-in `ESPAL` global for the lite build. The pure ID/size **formatting** is host-testable; the hardware reads are isolated behind a seam.

- [ ] **Step 1: Write the failing test for ID formatting + flash-size reporting**

`test/test_espal_lite/test_espal_lite.cpp`:

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../src/lite/espal_lite_format.h"

TEST_CASE("short id is low 24 bits as 6 lowercase hex") {
  CHECK(lite_format_short_id(0x00ABCDEF12345678ULL) == "345678");
}
TEST_CASE("long id is full 64-bit unique id as 16 lowercase hex") {
  CHECK(lite_format_long_id(0x00ABCDEF12345678ULL) == "00abcdef12345678");
}
TEST_CASE("flash size passes through bytes") {
  CHECK(lite_flash_size_bytes(0x200000) == 2097152u);
}
```

- [ ] **Step 2: Run it; verify it fails to compile (missing header)**

```
pio test -e native -f test_espal_lite
```
Expected: FAIL — `espal_lite_format.h` not found.

- [ ] **Step 3: Implement the pure formatting unit**

`src/lite/espal_lite_format.h`:

```cpp
#pragma once
#include <stdint.h>
#include <string>
inline std::string lite__hex(uint64_t v, int width) {
  static const char *d = "0123456789abcdef";
  std::string s(width, '0');
  for (int i = width - 1; i >= 0; --i) { s[i] = d[v & 0xF]; v >>= 4; }
  return s;
}
inline std::string lite_format_short_id(uint64_t uid) { return lite__hex(uid & 0xFFFFFF, 6); }
inline std::string lite_format_long_id(uint64_t uid)  { return lite__hex(uid, 16); }
inline uint32_t    lite_flash_size_bytes(uint32_t reported) { return reported; }
```

- [ ] **Step 4: Add the test source to the native env and run; verify PASS**

In `platformio.ini` `[env:native]`, extend `build_src_filter` (header-only unit needs no extra source, but keep the suite discoverable):

```
pio test -e native -f test_espal_lite
```
Expected: PASS (3 assertions).

- [ ] **Step 5: Implement the device-facing shim**

`src/lite/espal_lite.h`:

```cpp
#pragma once
#ifdef OPENEVSE_LITE
#include <Arduino.h>
#include <string>
class EspalLite {
public:
  void begin();
  uint32_t getFreeHeap();
  String getShortId();   // 6 hex of EFM32 DEVINFO unique id
  String getLongId();    // 16 hex of EFM32 DEVINFO unique id
  uint32_t getFlashChipSize();
  String getChipInfo();
  void reset();          // NVIC_SystemReset via LibreTiny
  void eraseConfig();    // clear the LittleFS config region (Task 4 owns the store)
};
extern EspalLite ESPAL;
#endif
```

`src/lite/espal_lite.cpp` — implement using LibreTiny/EFM32 primitives: read the 64-bit unique id from EFM32 `DEVINFO->UNIQUEL/UNIQUEH` (or LibreTiny's chip-id API) and feed `lite_format_short_id`/`lite_format_long_id`; `getFreeHeap()` → LibreTiny `lt_heap_get_free()` (or `xPortGetFreeHeapSize()`); `getFlashChipSize()` → `lite_flash_size_bytes(0x200000)` for the WGM160P 2 MB part (confirm against LibreTiny's flash API at integration); `reset()` → `lt_reboot()` / `NVIC_SystemReset()`; `getChipInfo()` → a fixed `"EFM32GG11B820/WF200"` string; `eraseConfig()` delegates to the Task-4 store. These are device reads with no host-test surface; the testable formatting is covered by Step 1.

- [ ] **Step 6: Commit**

```bash
git add src/lite/espal_lite.h src/lite/espal_lite.cpp src/lite/espal_lite_format.h test/test_espal_lite/ platformio.ini
git commit -m "lite: EFM32 ESPAL shim + host-tested id/flash formatting"
```

---

### Task 3: TRNG-backed RNG seam

**Files:**
- Create: `src/lite/lite_random.h`, `src/lite/lite_random.cpp`
- Create: `test/test_lite_random/test_lite_random.cpp`

The firmware uses `esp_fill_random()` in 4 places and `mongoose_rng.cpp` defines `mg_ssl_if_mbed_random()` over it. TLS is OFF this slice (`MG_ENABLE_SSL=0`), so the Mongoose RNG symbol may be unreferenced — but the seam is still needed for any random bytes (e.g. a boot/session id) and must compile without `esp_random.h`. Provide a `lite_random_bytes()` with an injectable backend so the contract is host-testable.

- [ ] **Step 1: Write the failing test**

`test/test_lite_random/test_lite_random.cpp`:

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../src/lite/lite_random.h"

TEST_CASE("fills exactly len bytes via injected backend") {
  uint8_t buf[8] = {0};
  lite_random_set_backend([](uint8_t *b, size_t n){ for (size_t i=0;i<n;i++) b[i]=0xA5; });
  lite_random_bytes(buf, sizeof buf);
  for (uint8_t b : buf) CHECK(b == 0xA5);
}
TEST_CASE("zero length is a no-op and does not crash") {
  lite_random_set_backend([](uint8_t*, size_t){});
  lite_random_bytes(nullptr, 0);
  CHECK(true);
}
```

- [ ] **Step 2: Run it; verify it fails (missing header)**

```
pio test -e native -f test_lite_random
```
Expected: FAIL — `lite_random.h` not found.

- [ ] **Step 3: Implement the seam**

`src/lite/lite_random.h`:

```cpp
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef void (*lite_random_backend_t)(uint8_t *buf, size_t len);
void lite_random_set_backend(lite_random_backend_t fn);  // test/integration injection
void lite_random_bytes(uint8_t *buf, size_t len);
```

`src/lite/lite_random.cpp`:

```cpp
#include "lite_random.h"
static lite_random_backend_t s_backend = nullptr;
void lite_random_set_backend(lite_random_backend_t fn) { s_backend = fn; }
void lite_random_bytes(uint8_t *buf, size_t len) {
  if (len == 0 || buf == nullptr) return;
  if (s_backend) { s_backend(buf, len); return; }
  for (size_t i = 0; i < len; i++) buf[i] = 0;  // safe default until backend wired
}
```

- [ ] **Step 4: Run the test; verify PASS**

```
pio test -e native -f test_lite_random
```
Expected: PASS (2 cases).

- [ ] **Step 5: Wire the EFM32 TRNG0 backend (device side)**

In the lite boot path (referenced by Task 6's `main_lite.cpp`), call `lite_random_set_backend(&efm32_trng_fill)` at startup, where `efm32_trng_fill` reads EFM32 `TRNG0` (the entropy source PR #387 added for Secure Link). Guard `mongoose_rng.cpp` so its `#include <esp_random.h>` body is excluded under `OPENEVSE_LITE` (TLS is off, so no replacement symbol is required this slice):

```cpp
// top of src/mongoose_rng.cpp
#ifndef OPENEVSE_LITE
#include <esp_random.h>
extern "C" int mg_ssl_if_mbed_random(void *ctx, unsigned char *buf, size_t len) {
  (void) ctx; esp_fill_random(buf, len); return 0;
}
#endif
```

- [ ] **Step 6: Commit**

```bash
git add src/lite/lite_random.h src/lite/lite_random.cpp test/test_lite_random/ src/mongoose_rng.cpp
git commit -m "lite: TRNG-backed random seam (host-tested) + gate esp_random out of lite"
```

---

### Task 4: Config persistence on LittleFS

**Files:**
- Create: `src/lite/lite_config_store.cpp`, `src/lite/lite_config_store.h`
- Reference: `src/app_config.cpp:721-776` (the `LittleFS` + `esp_partition` paths)

Slice-1 minimum: load WiFi SSID/PSK from LittleFS (defaults if blank), persist updates, and provide the `eraseConfig` the ESPAL shim delegates to. The full `ConfigJson`/`app_config` surface is NOT brought up this slice — only the WiFi-cred subset needed to associate across reboot.

- [ ] **Step 1: Define the store interface**

`src/lite/lite_config_store.h`:

```cpp
#pragma once
#ifdef OPENEVSE_LITE
#include <Arduino.h>
struct LiteWifiConfig { String ssid; String pass; };
bool lite_config_begin();                       // mount LittleFS; format if absent
bool lite_config_load_wifi(LiteWifiConfig &out); // false if no creds stored yet
bool lite_config_save_wifi(const LiteWifiConfig &in);
void lite_config_erase();                        // wipe the config file (eraseConfig)
#endif
```

- [ ] **Step 2: Implement over LibreTiny LittleFS**

`src/lite/lite_config_store.cpp` — `lite_config_begin()` calls LibreTiny `LittleFS.begin(true)` (format-on-fail); creds stored as a tiny JSON doc (`ArduinoJson`, already a dep) at `/lite_wifi.json`; `lite_config_erase()` removes that file (this replaces the ESP-only `esp_partition` erase at `app_config.cpp:723` for the lite build). Reads/writes use `LittleFS.open(path, "r"/"w")`.

- [ ] **Step 3: Build-check (compile only; host has no LittleFS)**

The store is device-only (LittleFS is not in the native env). Verify it compiles in the lite env once Task 6 includes it; no native test (no host filesystem seam this slice). Note this explicitly in the commit so the gap is visible.

- [ ] **Step 4: HW checkpoint is folded into Task 6** (creds survive reboot) — no standalone flash here.

- [ ] **Step 5: Commit**

```bash
git add src/lite/lite_config_store.h src/lite/lite_config_store.cpp
git commit -m "lite: LittleFS WiFi-cred store + eraseConfig (device-only; no host FS seam)"
```

---

### Task 5+6 (merged): EVSE-core lite bring-up — boot path + RAPI + live `/status`

**Author decisions (2026-06-12) baked into this task:**
- **Keep the real EVSE core, drop FakeEVSE.** The JuiceBox has a real OpenEVSE controller; `EvseManager` talks to it over RAPI. No `-D FAKE_EVSE`, no `fake_evse*` (those files aren't even on this branch).
- **RAPI = USART0 LOC1 (PE7=TX/PE6=RX) @ 9600 8N1**, which is the *only* USART the LibreTiny EFM32 fork wires — the same line the debug console used. Decision: **RAPI for real, no separate console this slice.** `RAPI_PORT` is `Serial` reconfigured to 9600. Observability is WiFi + `curl /status`, not the UART.
- **Silence everything that would corrupt RAPI framing on that shared UART:** no `Serial.print*` debug in the lite boot path, and turn LibreTiny's own `LT_*` logging off for this env (it routes to USART0 too).
- **Persistence deferred.** No LittleFS (the fork has none — it ships `Preferences`). `lite_config_store.cpp` is excluded from this build; creds come from the `LITE_WIFI_SSID/PASS` build flags. Revert `EspalLite::eraseConfig()` to a no-op stub (drop the `lite_config_store.h` include) so the excluded store doesn't break the link. A `Preferences`-backed store is a later slice.

**Files:**
- Create: `src/lite/main_lite.cpp` (the entry — instantiates the EVSE core), `src/lite/web_server_lite.{h,cpp}`, `src/lite/app_config_lite.cpp` (tiny config shim)
- Modify: `src/lite/espal_lite.cpp` (UID fix + revert eraseConfig stub), `platformio.ini` (`[env:openevse_lite]` lib_deps + build_src_filter)
- Delete: `src/lite/spike_main.cpp`
- Reference: `src/lite/spike_main.cpp` (vendored-Mongoose `mg_mgr`/`mg_bind`/`ev_handler` idiom + LibreTiny `IPAddress` raw-bytes note), `src/main.cpp:77,87,260-261` (`EvseManager evse(RAPI_PORT, eventLog)`, `rapiSender`, loop ticks), `src/web_server.cpp` (the `/status` JSON shape / `json_serialize` helpers)

- [ ] **Step 1: Fix the espal UID + revert eraseConfig**

In `src/lite/espal_lite.cpp`: replace the non-existent `lt_cpu_get_uid64()` with the fork's real accessor — the full 64-bit is `((uint64_t)DEVINFO->UNIQUEH << 32) | DEVINFO->UNIQUEL` (include `<em_device.h>`/`<libretiny.h>` as the other EFM32 reads do; `lt_cpu_get_unique_id()` only returns the low 24 bits). Feed that into `lite_format_short_id`/`lite_format_long_id`. Revert `EspalLite::eraseConfig()` to a no-op stub and remove the `#include "lite_config_store.h"` (persistence is out of this slice).

- [ ] **Step 2: app_config lite shim**

`src/lite/app_config_lite.cpp` (gated `#ifdef OPENEVSE_LITE`) provides the *only* `app_config` symbols the EVSE core links: `config_threephase_enabled()` (return `false`) and `config_default_state()` (return the firmware default — check `app_config.h` for the constant, typically `EVSE_STATE_ACTIVE`/`OPENEVSE_STATE_*`). Add more symbols **only** as the linker names them. The real `app_config.h` header is still included by `evse_man.h`; if it transitively needs `ConfigJson`, keep `jeremypoulter/ConfigJson` in lib_deps for the header to *compile*, but do NOT bring in `src/app_config.cpp`.

- [ ] **Step 3: Minimal Mongoose server + live `/status`**

`web_server_lite.{h,cpp}`: vendored-Mongoose manager on `:80` (same idiom as the spike — 4-arg userdata handler, `mg_bind(&mgr,"80",h,NULL)`, `mg_set_protocol_http_websocket`, `MG_F_SEND_AND_CLOSE`). `web_server_lite_begin(EvseManager &evse)` stashes the evse ref; one handler:
- `GET /status` → JSON of the **live EvseManager** state via `ArduinoJson` — at minimum `state`, `amp`, `voltage`, `pilot`, `vehicle`, plus `free_heap`/`uptime`. Reuse `src/web_server.cpp`'s status fields; if the builder isn't separable, hand-pick the core getters from `EvseManager`/`EvseMonitor` (do not pull in the whole `web_server.cpp`).
- `GET /` → `200 "openevse-lite"`; else 404.
`web_server_lite_loop()` → `mg_mgr_poll(&mgr, 0)`.

- [ ] **Step 4: The lite boot path (no console)**

`main_lite.cpp`: globals `EventLog eventLog;` + `EvseManager evse(RAPI_PORT, eventLog);` (define `RAPI_PORT` → `Serial` for the lite build, e.g. a small `#ifdef OPENEVSE_LITE #define RAPI_PORT Serial`). `setup()`: `Serial.begin(9600)` (RAPI baud, 8N1 default — this UART is the controller link, **no debug prints to it**) → `ESPAL.begin()` → `WiFi.begin(LITE_WIFI_SSID, LITE_WIFI_PASS)` → wait `WL_CONNECTED` (silent — no dots) → `evse.begin()` → `web_server_lite_begin(evse)`. `loop()`: `web_server_lite_loop();` + `MicroTask.update();`/`evse` + `rapiSender` ticks (mirror `src/main.cpp:260-261`). TRNG/`lite_random` backend stays unwired (TLS off). No `Serial.print*` anywhere.

- [ ] **Step 5: Env — lib_deps + source filter**

`[env:openevse_lite]`: set `lib_deps` to the EVSE-core subset (no Mongoose/MQTT/OCPP libs):
```ini
lib_deps =
  bblanchon/ArduinoJson@6.20.1
  jeremypoulter/OpenEVSE@0.0.15
  jeremypoulter/MicroTasks@0.0.4
  jeremypoulter/Micro Debug@0.0.5
  jeremypoulter/ConfigJson@0.0.6
  jeremypoulter/StreamSpy@0.0.2
```
(add/remove only as the link demands). Silence LibreTiny logging via build_flags (e.g. `-D LT_LOGLEVEL=4` / the fork's "no logger" define — confirm the exact macro in the fork's `lt_logger`/`lt_defs.h`). Source filter:
```ini
build_src_filter =
  -<*>
  +<lite/>
  -<lite/spike_main.cpp>
  -<lite/lite_config_store.cpp>
  +<evse_man.cpp> +<evse_monitor.cpp> +<event_log.cpp> +<openevse.cpp>
```
Delete `src/lite/spike_main.cpp`. Add only the minimal EVSE objects each `undefined reference` names — **never** web/mqtt/ocpp/divert/ha/tsdb/lcd/net_manager.

- [ ] **Step 6: Iterate to a clean local link (implementer runs)**

```
pio run -e openevse_lite 2>&1 | grep -E "undefined reference|error:" || echo LINK_OK
```
Expected: `LINK_OK`. Record the final `build_src_filter` + `lib_deps` in the commit. If a missing symbol names a heavy ESP-only subsystem, **stop and escalate** — do not widen the filter to pull it in; the EVSE core should need only its own objects + the libs above.

- [ ] **Step 7: HW acceptance (author runs) — the slice goal**

```
pio run -e openevse_lite -t upload
curl http://<ip>/status
```
No serial console (USART0 is the RAPI line). Expected: board joins WiFi (appears on the LAN); `curl /status` returns live EVSE JSON. With the controller wired to PE7/PE6 @ 9600, `state`/`amp`/`voltage` reflect the real OpenEVSE; with it unwired, the EVSE reads disconnected but the server still answers.

- [ ] **Step 8: Commit**

```bash
git add src/lite/main_lite.cpp src/lite/web_server_lite.cpp src/lite/web_server_lite.h src/lite/app_config_lite.cpp src/lite/espal_lite.cpp platformio.ini
git rm src/lite/spike_main.cpp
git commit -m "lite: EVSE-core bring-up — boot path + RAPI (USART0 9600) + live /status"
```

---

### Task 7: Flash-budget verification + slice acceptance

**Files:**
- Create: `docs/superpowers/notes/2026-06-12-openevse-lite-slice1-results.md`

- [ ] **Step 1: Capture the image size (author runs)**

```
pio run -e openevse_lite 2>&1 | tail -20
```
Record the reported program/flash size. Expected: the app image fits the 2 MB part with headroom after the WF200 firmware blob + config region. If it does not, the LITE gate missed something — list the largest objects (`riscv*/arm*-size` on the ELF) and trim before declaring the slice done.

- [ ] **Step 2: Record results**

Write the notes file with: Route 1 confirmed (Y/N), final `build_src_filter`, image size + budget headroom, the `/status` JSON captured from hardware, and any LibreTiny-fork changes needed (lwipopts deltas) so they can fold back into PR #387.

- [ ] **Step 3: Commit**

```bash
git add docs/superpowers/notes/2026-06-12-openevse-lite-slice1-results.md
git commit -m "lite: slice-1 results — Route 1 confirmed, flash budget, /status capture"
```

---

## Self-review

**Spec coverage:** env `openevse_lite` (T1/T5), `-D OPENEVSE_LITE`+`JUICEBOX_40`+`FAKE_EVSE` (T1/T5), 2 MB layout/budget (T7), ESPAL `src/` shim (T2), TRNG RNG (T3), Mongoose Route 1 + `CS_P_CUSTOM` + lwipopts (T1), minimal `/status` not full web_server (T6), LittleFS config + esp_partition swap (T4), STA pre-provisioned creds (T1/T6), gated-off list (T5 filter + `MG_ENABLE_SSL=0`). All spec items mapped.

**Placeholder scan:** no "TBD/TODO/handle errors" left. Two honest, explicitly-flagged gaps remain by necessity: the LibreTiny fork URL/board/lwipopts path is a NEEDS_CONTEXT in T1 S1 (author-held, not guessable), and device-only files (espal_lite.cpp device reads, lite_config_store.cpp, EFM32 TRNG fill) have no host-test seam — their testable logic was split into host-tested pure units (espal_lite_format.h, lite_random seam) and the device portions are validated at the HW checkpoints. These are real-world constraints, not lazy placeholders.

**Type consistency:** `lite_format_short_id/long_id`, `lite_flash_size_bytes`, `lite_random_bytes`/`lite_random_set_backend`/`lite_random_backend_t`, `LiteWifiConfig`/`lite_config_*`, `EspalLite`/`ESPAL`, `web_server_lite_begin(evse)`, `build_status_json(JsonDocument&, EvseManager&)` are used consistently across tasks.
