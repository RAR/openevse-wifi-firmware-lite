#pragma once
#ifdef OPENEVSE_LITE
// ============================================================================
//  CLRC663 RFID — host pin map (the ONE place to edit when the bench tracing is
//  finalized). The reader hangs off a bit-banged SPI bus on WGM160P (EFM32GG11)
//  GPIOs; the JuiceBox app does NOT use the hardware SPI peripheral. Pins are
//  EFM32 port/pin pairs for the em_gpio API (gpioPortA..F, 0..15).
//
//  STATUS (2026-06-17): NOT YET CONFIRMED — feature is compiled but inert until
//  LITE_RFID_PINS_CONFIRMED is defined (see below). What we know from the open
//  Gecko OS shell + the daughterboard connector (memory: juicebox-rfid-clrc663-
//  interface, juicebox-gecko-os-shell):
//
//    connector  signal        EFM32        notes
//    ---------  ------------  -----------  ---------------------------------------
//    pin 3      PF10           PF10         output (Gecko GPIO21)  -> MOSI or CS
//    pin 4      MISO (input)   PB4 or PC5   the only input-pullup GPIOs (9 / 20)
//    pin 5      SCLK (output)  TBD          you tagged it SCLK; exact pin pending
//    pin 6      PF11           PF11         output (Gecko GPIO22)  -> MOSI or CS
//
//  *** CONFLICT TO RESOLVE FIRST ***  main_lite.cpp drives PF11 as the ATmega
//  RESET line (active-low, continuity-confirmed 2026-06-13; holding it high fixed
//  the flapping-comms bug). PF11 therefore CANNOT also be an RFID SPI line. So the
//  connector's "pin 6 = PF11" reading must be re-checked tomorrow — either it is a
//  different pin, or the RFID bus doesn't use PF11. DO NOT assign any RFID signal
//  to PF11 until this is settled, or boot will reset/relemax the safety MCU.
//
//  To bring it up: trace pin4 + pin5 to their EFM32 pins, decide MOSI-vs-CS on the
//  PF10/PF11 pair (read CLRC663 VERSION==0x18; swap if it NAKs), fill the six
//  macros below, and add `-D LITE_RFID_PINS_CONFIRMED` to the device build_flags.
// ============================================================================

// --- CONFIRMED pinout (2026-06-18, live VERSION==0x18 read via J-Link scan) ---------
// The CLRC663 hangs off a SHARED bit-bang SPI bus (mode 0). These four are the WGM-side
// pins, verified by lite_rfid_scan.cpp reading VERSION==0x18:
//   SCLK=PC4  MOSI=PB3  MISO=PB4  CS=PF10
// PE5 (continuity-found, 330Ω series) is NOT SPI — almost certainly the reader ENABLE/RESET
// (must be driven HIGH to run; hi-Z left the reader powered down). See LITE_RFID_HAS_PDOWN note.
//
// NO BUS CONTENTION (confirmed): the WGM reads VERSION==0x18 with the ATmega RUNNING (PF11 high)
// just as well as with it in reset → the ATmega does NOT drive this reader bus (its SPI-master
// code is vestigial/other). The WGM is the sole reader master → lite can bit-bang it anytime,
// no arbitration. Before flipping LITE_RFID_PINS_CONFIRMED on: wire the ENABLE (PE5, drive HIGH
// to run — active-high, opposite the PDOWN hook below) and HW-validate read-UID + allowlist + gate.
#ifndef LITE_RFID_SCLK_PORT
#define LITE_RFID_SCLK_PORT  gpioPortC      // PC4 (confirmed)
#define LITE_RFID_SCLK_PIN   4
#endif
#ifndef LITE_RFID_MOSI_PORT
#define LITE_RFID_MOSI_PORT  gpioPortB      // PB3 (confirmed)
#define LITE_RFID_MOSI_PIN   3
#endif
#ifndef LITE_RFID_MISO_PORT
#define LITE_RFID_MISO_PORT  gpioPortB      // PB4 (confirmed)
#define LITE_RFID_MISO_PIN   4
#endif
#ifndef LITE_RFID_CS_PORT
#define LITE_RFID_CS_PORT    gpioPortF      // PF10 (confirmed)
#define LITE_RFID_CS_PIN     10
#endif

// CLRC663 enable/reset = PE5 — ACTIVE-HIGH: drive HIGH to run. (Confirmed: hi-Z left the reader
// powered down; driving it high is what let the WGM read VERSION==0x18.) This is a drive-HIGH
// enable, NOT the active-low PDOWN semantics, so it uses its own LITE_RFID_HAS_ENABLE hook.
#ifndef LITE_RFID_ENABLE_PORT
#define LITE_RFID_HAS_ENABLE
#define LITE_RFID_ENABLE_PORT gpioPortE     // PE5
#define LITE_RFID_ENABLE_PIN  5
#endif

// Bit-bang half-clock (microseconds). ~2 us => ~250 kHz; conservative for bring-up.
#ifndef LITE_RFID_SPI_HALF_US
#define LITE_RFID_SPI_HALF_US 2
#endif

// Card-poll cadence (ms). The reader is polled (no IRQ pin on the connector).
#ifndef LITE_RFID_POLL_INTERVAL_MS
#define LITE_RFID_POLL_INTERVAL_MS 300
#endif

#endif // OPENEVSE_LITE
