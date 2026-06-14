# OpenEVSE Lite — JuiceBox 40 / LibreTiny Port (Slice 1) Design

**Status:** Approved design, ready for implementation planning.
**Date:** 2026-06-12
**Branch:** `feature/juicebox-lite` (off upstream `master` @ `ae3406c`).

## Goal

Get the OpenEVSE WiFi-companion firmware booting and serving `/status` over
plain HTTP on the **Enel X JuiceBox 40** (Silicon Labs **WGM160P** SiP), running
against the in-firmware **FakeEVSE** stub — proving the LibreTiny/EFM32 platform
end-to-end before any safety-critical charging control or feature breadth.

Acceptance: power on → WF200 STA associates + DHCP lease → `curl http://<ip>/status`
returns FakeEVSE-backed JSON → the linked app image fits 2 MB with headroom.

## Why this is a port, not a feature trim

"Lite, 2 MB flash" turned out to mean a **new silicon target**, not a
feature-stripped ESP32 build. The target is the WGM160P (EFM32GG11B820
Cortex-M4F, 2 MB flash, ~512 KB RAM, on-package WF200 WiFi over SDIO with
*enforced* Secure Link), reached via the author's LibreTiny port
(libretiny-eu/libretiny PR #387). The JuiceBox has **no separate OpenEVSE/RAPI
controller** — the EFM32 is the charger brain — so the firmware's EVSE layer
(`EvseManager` → `OpenEVSE` RAPI lib over `RAPI_PORT`) has nothing to talk to.
Slice 1 sidesteps that by using **FakeEVSE** (`-D FAKE_EVSE`) as the EVSE source.
Real J1772 pilot/relay/GFCI control is a deliberately separate, later effort.

## Coupling survey (what makes this tractable)

- **`src/` is barely ESP-IDF-coupled.** All chip access goes through **ESPAL**
  (~9 call sites: `getFreeHeap` ×25, `getShortId`/`getLongId` ×4 each,
  `eraseConfig` ×3, `reset` ×2, `getFlashChipSize`, `getChipInfo`, `begin` ×1),
  plus a short tail of direct includes: `esp_random.h` (×4, the RNG),
  `esp_wifi.h` (×1), `esp_ota_ops.h` (×1). The port is mostly *library-level*.
- **The web GUI is compiled into the image** (`src/web_static/*.h`) — no
  filesystem needed to serve it.
- **Networking spine = ArduinoMongoose** (Mongoose 6). On ESP32 it uses BSD
  sockets, not the raw lwIP driver (see below). The ArduinoMongoose wrapper's
  ESP coupling is trivial: `arduino.cpp` is a no-op poll-schedule stub; the
  `esp_` hits in `MongooseHttpServer.cpp` / `MongooseWebSocketClient.cpp` are
  false positives (`resp_code`/`resp_status_msg`); the only real Arduino
  coupling is `MongooseCore.cpp` reading `WiFi.dnsIP(0)`.
- **Config** uses `EEPROM` + `ConfigJson` + `LittleFS` + one `esp_partition`
  erase call — all have LibreTiny equivalents.

## The networking spine — the make-or-break, resolved on paper

On ESP32, `mongoose.h` sets `MG_LWIP 1` **but** `MG_NET_IF = MG_NET_IF_SOCKET`
(mongoose.h:553-555). The actual I/O is a `select()` loop over non-blocking BSD
sockets (`fcntl(O_NONBLOCK)` mongoose.c:3665-66; `getaddrinfo` 2547;
`MG_NET_IF_SOCKET` 3433/4222). `MG_LWIP 1` only pulls lwIP *compat* shims
(timeval, a TCP keepalive helper). So the real dependency is **lwIP's socket
layer**, not Mongoose's fiddly raw `tcp_pcb` driver.

The port therefore reduces to three things:

1. **A custom Mongoose platform header** (`CS_P_CUSTOM`, or a new conditional
   block) that sets `MG_NET_IF = MG_NET_IF_SOCKET`, `MG_LWIP 1`, and includes
   the right arm-none-eabi newlib + lwIP headers (`lwip/sockets.h`,
   `lwip/netdb.h`, `sys/stat.h`, `sys/time.h`, `machine/endian.h`, …).
2. **LibreTiny's lwIP must expose the socket API**: `LWIP_SOCKET=1`, `select()`,
   non-blocking via `fcntl`/`FIONBIO`, enough `MEMP_NUM_NETCONN` for a listener
   plus a few client connections, and `LWIP_DNS` + `netdb` for `getaddrinfo`.
3. **`WiFi.dnsIP()`** from LibreTiny's Arduino WiFi library for `MongooseCore`.

**Strategic advantage:** the author *owns* the LibreTiny EFM32 port (PR #387),
so `lwipopts.h` is ours to set. Enabling `LWIP_SOCKET`/`select`/`LWIP_DNS` and
tuning socket counts is a change we make in our own lwIP config, not a fight
against a vendor SDK. That is what turns this from "unknown risk" into "a known
config plus a custom platform header."

### Route 1 (recommended) — `MG_NET_IF_SOCKET` over lwIP sockets
The exact path ESP32 uses. Needs `LWIP_SOCKET=1` + `select` in our lwipopts and
a `CS_P_CUSTOM` header. Least Mongoose surgery; reuses the battle-tested socket
netif. RAM cost: one lwIP netconn + socket per live connection.

### Route 2 (escape hatch) — `MG_NET_IF_LWIP_LOW_LEVEL`
Mongoose's raw `tcp_pcb`/`udp_pcb` callback driver (mongoose.c:14863+). No
sockets required; lower RAM; but it is the less-maintained driver and must be
pinned to the lwIP `tcpip_thread`. Use only if `LWIP_SOCKET` cannot be enabled
cleanly. We expect to confirm Route 1 on the first task and never reach here.

## Slice 1 scope

### In scope
- New `[env:openevse_lite]`: `platform = libretiny` (pinned to the author's fork
  carrying PR #387), `board = wgm160p-juicebox-40`, `framework = arduino`. Env
  name is generic (`openevse_lite`) so other LibreTiny boards can follow; the
  board is identified separately by `-D JUICEBOX_40`.
- New `-D OPENEVSE_LITE` umbrella gate + `-D JUICEBOX_40` board flag.
- `-D FAKE_EVSE` on (EVSE source).
- A 2 MB single-app, no-dual-OTA flash layout (LibreTiny FAL/FlashDB based),
  leaving room for the WF200 firmware blob + a config region.
- **EFM32/LibreTiny backing for the ~9 ESPAL calls = a thin `src/` shim** under
  `-D OPENEVSE_LITE` (self-contained in this repo, fast to iterate; promotable to
  the ESPAL fork later). NOT a cross-repo ESPAL change in slice 1.
- TRNG0-backed RNG to replace the `esp_random.h` calls (minimal in slice 1; no
  TLS yet).
- ArduinoMongoose on LibreTiny lwIP via Route 1; one plain-HTTP listener on `:80`.
- A **minimal route set** (`/status`, optional `/` health) under the LITE gate —
  NOT the full `web_server.cpp` — fed by the existing status builder against
  FakeEVSE.
- `app_config` load/persist of WiFi creds to EFM32 flash via **LibreTiny
  LittleFS** (the firmware already leans on LittleFS — least divergence); the
  `esp_partition` erase path swapped for a LibreTiny flash erase.

### WiFi provisioning assumption (softAP dependency)
LibreTiny PR #387 is **STA-only** today (softAP is a follow-on the author is
actively building). Slice 1 therefore assumes STA credentials are
**pre-provisioned** (compiled-in or written to config out of band) and connects
in STA mode. AP-mode provisioning / captive portal rides on the in-progress
softAP work and is explicitly NOT a slice-1 dependency — slice 1 must not block
on it.

### Gated OFF in slice 1
OCPP, divert, Tesla, emoncms, Home Assistant, tsdb, RFID, LCD/TFT, **OTA**, and
**TLS** (`MG_ENABLE_SSL=0`). The full `web_server.cpp` route surface is excluded
in favor of the minimal `/status` route.

### Out of scope (later specs)
TLS/HTTPS (mbedTLS 3.x + TRNG), MQTT, the full web UI / all routes, real J1772
pilot/relay/GFCI control, and OTA / dual-bank.

## Build & test plan shape

- **Task 1 is a networking spike** (before any OpenEVSE code): confirm/enable
  `LWIP_SOCKET=1` + `select` + `LWIP_DNS` in the LibreTiny EFM32 `lwipopts.h`;
  write the `CS_P_CUSTOM` Mongoose platform header; stand up a bare `mg_mgr` +
  one HTTP listener returning `"hello"` on `:80` — no app_config, no FakeEVSE.
  Serving a request on the JuiceBox proves Route 1; failing tells us on day one
  to pivot to Route 2, before anything is built on top.
- Subsequent tasks layer in: ESPAL/EFM32 shim + TRNG → build env + 2 MB layout →
  WiFi connect + config persist → FakeEVSE wiring → `/status` route.
- **Native doctest** (runs on the dev host, no hardware) covers any new pure
  logic: EFM32 short/long-ID formatting, flash-size reporting, and any
  status-shaping helpers extracted along the way.
- **Hardware acceptance** on the author's JuiceBox 40 (the only board on hand):
  boot + serial logs, WiFi + DHCP, `curl /status` returns FakeEVSE JSON, and the
  link map shows the app fits 2 MB with headroom after the WF200 blob.

## Risks

1. **Mongoose Route 1 on LibreTiny lwIP** — retired by Task 1; Route 2 fallback.
2. **RAM (512 KB)** with lwIP + WF200 driver + FreeRTOS + Mongoose live at once;
   watch socket/connection buffer counts (`MEMP_NUM_*`, Mongoose recv buffers).
3. **Flash budget (2 MB)** after the WF200 firmware blob + Secure-Link mbedtls;
   the LITE gate exists to keep the app well under budget.
4. **Build depends on the unmerged LibreTiny fork** (PR #387 branch) — pin it
   explicitly in `platformio.ini`.

## Resolved choices

- **Env name:** `openevse_lite` (generic; board distinguished by `-D JUICEBOX_40`).
- **ESPAL seam:** thin `src/` shim under `-D OPENEVSE_LITE` (not a cross-repo
  ESPAL change in slice 1).
- **Config store:** LibreTiny LittleFS for the config JSON.
- **WiFi:** STA-only with pre-provisioned creds; AP/softAP provisioning is out of
  scope and tracked against the author's in-progress softAP work.
