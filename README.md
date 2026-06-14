# OpenEVSE WiFi Firmware — Lite

Companion WiFi firmware for the **Enel X JuiceBox 40**, ported from the OpenEVSE
ESP32 WiFi firmware. It runs on the JuiceBox's **WGM160P** WiFi co-processor
(Silicon Labs **EFM32GG11** Cortex-M4F + **WF200** WiFi) via
[LibreTiny](https://github.com/RAR/libretiny), and talks to the on-board
**ATmega** safety MCU over the JuiceBox `$`-protocol.

> **Not an ESP32 target.** There is no ESP-IDF in this build. This repo was split
> out of the OpenEVSE ESP32 firmware precisely because the platform, toolchain,
> and flash flow share nothing with the ESP32 line — only design lineage.

## What it does

A compact EVSE controller + local web/API surface that mirrors the OpenEVSE
local HTTP contract (so the Home Assistant integration and similar clients work
unchanged), sized to the JuiceBox's dual-OTA flash budget:

- **JuiceBox `$`-protocol backend** — read state + heartbeat/keepalive, charge
  control via `$AL` active-limit, fault decode from `$ES`/`$MD`/`$WR`.
- **Control seam** — claim-based arbitration (manual override > schedule >
  divert), 6 A J1772 floor enforced in hardware terms.
- **Local API** — `/status` (+ `/ws` push), `/config`, `/override`, `/schedule`,
  and a `POST /status` feed-in for solar/grid/shaper data.
- **Solar divert + load shaping** — full-parity transcription of the OpenEVSE
  divert / current-shaper logic.
- **Scheduler, wall-clock (SNTP), lifetime energy totals, RGB status LED.**
- **WiFi provisioning** — stored creds + softAP setup flow (`/scan`, `/connect`);
  no compile-time credentials. Serves the
  [openevse-gui-lite](https://github.com/RAR/openevse-gui-lite) bundle.

## Layout

```
src/lite/        firmware sources (pure logic units + device glue)
test/test_lite_* host doctest suites for the pure units
lib/MongooseLite  vendored Mongoose 6.x core (HTTP/WS, SSL compiled out)
scripts/          lite_flash.sh (SWD/J-Link flash), lite_exclude_sources.py
docs/             design specs + implementation plans, UI backend contract
```

The codebase follows a **pure-unit + thin-glue** pattern: logic lives in
header-light, native-testable units (`#pragma once` + `<stdint.h>`); device I/O
lives in glue (`main_lite.cpp`, `web_server_lite.cpp`, `juicebox_backend.cpp`).

## Build

```sh
# Host unit tests (pure logic units)
pio test -e native

# Device firmware
pio run -e openevse_lite
```

The device platform is pinned to the LibreTiny fork with the EFM32GG11 + WF200
support (`platform = https://github.com/RAR/libretiny.git#feature/silabs-efm32gg11-ota`).

## Flash (SWD / J-Link)

```sh
pio run -e openevse_lite -t upload   # runs scripts/lite_flash.sh
```

> **Never `openocd program raw_firmware.elf`** — its `p_paddr=0` LOAD segment
> paints the ELF header over the bootloader at `0x0` and bricks boot
> (double-fault). The flash script writes the two flat images at their correct
> offsets: `bootloader.bin → 0x00000000`, `firmware.bin → 0x00008000`.

## First-boot provisioning

A freshly-flashed unit has no stored WiFi credentials, so it starts an **open
softAP `OpenEVSE-Lite-<id>`** at `192.168.4.1`. Join it, open `http://192.168.4.1/`,
pick your network, and the device saves the credentials and reboots onto it.
Credentials persist in the KVS partition across firmware reflashes.

## Flash budget

Dual-OTA: the real application ceiling is the **960 KB OTA slot (983040 B)** —
PlatformIO's percentage (computed against ~2 MB) reads about half the true figure.
Keep `firmware.bin / 983040` comfortably under 100 % with OTA margin.
