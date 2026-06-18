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

// --- placeholder defaults (deliberately NOT real pins; override before enabling) ---
// These exist only so lite_rfid.cpp compiles while disabled. They are never driven
// unless LITE_RFID_PINS_CONFIRMED is defined. None of them is PF11 (see conflict).
#ifndef LITE_RFID_SCLK_PORT
#define LITE_RFID_SCLK_PORT  gpioPortA
#define LITE_RFID_SCLK_PIN   0
#endif
#ifndef LITE_RFID_MOSI_PORT
#define LITE_RFID_MOSI_PORT  gpioPortF      // candidate: PF10 (connector pin 3)
#define LITE_RFID_MOSI_PIN   10
#endif
#ifndef LITE_RFID_MISO_PORT
#define LITE_RFID_MISO_PORT  gpioPortB      // candidate: PB4 (Gecko GPIO9, input-pullup)
#define LITE_RFID_MISO_PIN   4
#endif
#ifndef LITE_RFID_CS_PORT
#define LITE_RFID_CS_PORT    gpioPortA      // TBD: the other of PF10/PF11; NOT PF11
#define LITE_RFID_CS_PIN     1
#endif

// Optional CLRC663 PDOWN (power-down, active-high; drive LOW to run). Not seen on
// the 7-pin connector, so it may be hardwired. Leave LITE_RFID_HAS_PDOWN undefined
// unless tracing finds a host GPIO for it.
#ifndef LITE_RFID_PDOWN_PORT
#define LITE_RFID_PDOWN_PORT gpioPortA
#define LITE_RFID_PDOWN_PIN  2
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
