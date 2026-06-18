#pragma once
#ifdef OPENEVSE_LITE
#ifdef LITE_RFID_SCAN
// ============================================================================
//  CLRC663 RFID pin-scan (bench bring-up only; gated by -D LITE_RFID_SCAN).
//
//  We know the reader is a bit-banged SPI bus on WGM160P (EFM32GG11) GPIOs, but
//  not WHICH pins. This sweep uses the CLRC663 VERSION register (0x18) as ground
//  truth: it walks candidate SCLK/MOSI/MISO/CS combinations, reads VERSION for
//  each, and records the combo that returns 0x18. VERSION is readable on power-up
//  with no reset/init, so each probe is a single ~16-clock transfer — the whole
//  sweep finishes in well under a second.
//
//  Result is published in the g_rfid_scan_result global so it can be read back
//  over SWD (J-Link) after boot — no WiFi/console needed. Run it early in setup().
// ============================================================================
#include <stdint.h>

struct LiteRfidScanResult {
  uint32_t magic;        // 0 until the sweep finishes; LITE_RFID_SCAN_MAGIC when done
  uint8_t  found;        // 1 if a combo returned VERSION==0x18
  uint8_t  version;      // the matching VERSION byte (0x18 on success)
  uint8_t  iface;        // 0 none, 1 SPI, 2 I2C, 3 UART
  uint8_t  i2c_addr;     // matching 7-bit I2C address (iface==2)
  uint8_t  sclk_port, sclk_pin;   // SCL on I2C; host-TX on UART
  uint8_t  mosi_port, mosi_pin;   // SDA on I2C
  uint8_t  miso_port, miso_pin;   // host-RX on UART
  uint8_t  cs_port,   cs_pin;
  uint16_t combos_tried;
  uint16_t plausible;    // # combos that returned a byte other than 0x00/0xFF (MISO wiggled)
  uint16_t i2c_acks;     // # I2C addresses that ACKed (device present on the bus)
  uint16_t uart_frames;  // # framed UART byte responses seen (device replying)
  uint32_t baud;         // matching baud (iface==3)
};

#define LITE_RFID_SCAN_MAGIC 0x5CA9D01Du

extern volatile LiteRfidScanResult g_rfid_scan_result;

// Run the full candidate sweep, populate g_rfid_scan_result, and return.
void lite_rfid_scan_run();

#endif // LITE_RFID_SCAN
#endif // OPENEVSE_LITE
