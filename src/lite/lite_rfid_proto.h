#pragma once
#include <stddef.h>
#include <stdint.h>

// CLRC663 (NXP, == MFRC630 family) wire protocol — pure, no hardware. The JuiceBox
// RFID daughterboard reader is a CLRC663 on a bit-banged SPI bus off the WGM160P
// (see memory: juicebox-rfid-clrc663-interface). Register names/commands mirror the
// public iwanders/mfrc630 + Pimoroni drivers so the sequencing is verifiable against
// a known reference. Everything here is compiled into the native test env.

// ---- SPI address byte (datasheet Rev 3.8 Table 17) ----------------------------
// The first SPI byte is the address byte: bits7..1 = register, bit0 = R/W
// (1 = read, 0 = write). On a read the register value is returned on the *next*
// byte (MISO is delayed one byte: rx[0] is don't-care, rx[1] is the value).
static inline uint8_t clrc663_addr_read(uint8_t reg)  { return (uint8_t)((reg << 1) | 0x01); }
static inline uint8_t clrc663_addr_write(uint8_t reg) { return (uint8_t)((reg << 1) & 0xFE); }

// ---- Registers (addresses verbatim from the MFRC630/CLRC663 map) ---------------
enum {
  CLRC663_REG_COMMAND      = 0x00,
  CLRC663_REG_HOSTCTRL     = 0x01,
  CLRC663_REG_FIFOCONTROL  = 0x02,
  CLRC663_REG_WATERLEVEL   = 0x03,
  CLRC663_REG_FIFOLENGTH   = 0x04,
  CLRC663_REG_FIFODATA     = 0x05,
  CLRC663_REG_IRQ0         = 0x06,
  CLRC663_REG_IRQ1         = 0x07,
  CLRC663_REG_IRQ0EN       = 0x08,
  CLRC663_REG_IRQ1EN       = 0x09,
  CLRC663_REG_ERROR        = 0x0A,
  CLRC663_REG_STATUS       = 0x0B,
  CLRC663_REG_RXBITCTRL    = 0x0C,
  CLRC663_REG_RXCOLL       = 0x0D,
  CLRC663_REG_TCONTROL     = 0x0E,
  CLRC663_REG_T0CONTROL    = 0x0F,
  CLRC663_REG_T0RELOADHI   = 0x10,
  CLRC663_REG_T0RELOADLO   = 0x11,
  CLRC663_REG_T1CONTROL    = 0x14,
  CLRC663_REG_T1RELOADHI   = 0x15,
  CLRC663_REG_T1RELOADLO   = 0x16,
  CLRC663_REG_DRVMOD       = 0x28,  // base of the recommended-register block
  CLRC663_REG_TXCRCPRESET  = 0x2C,
  CLRC663_REG_RXCRCCON     = 0x2D,
  CLRC663_REG_TXDATANUM    = 0x2E,
  CLRC663_REG_VERSION      = 0x7F,
};

// ---- Commands (Command register values) ----------------------------------------
enum {
  CLRC663_CMD_IDLE         = 0x00,
  CLRC663_CMD_LOADKEY      = 0x02,
  CLRC663_CMD_MFAUTHENT    = 0x03,
  CLRC663_CMD_RECEIVE      = 0x05,
  CLRC663_CMD_TRANSCEIVE   = 0x07,
  CLRC663_CMD_LOADPROTOCOL = 0x0D,
  CLRC663_CMD_SOFTRESET    = 0x1F,
};

// ---- IRQ0 register bits (datasheet / MFRC630_IRQ0_*) ---------------------------
enum {
  CLRC663_IRQ0_SET     = 0x80,
  CLRC663_IRQ0_HIALERT = 0x40,
  CLRC663_IRQ0_LOALERT = 0x20,
  CLRC663_IRQ0_IDLE    = 0x10,
  CLRC663_IRQ0_TX      = 0x08,
  CLRC663_IRQ0_RX      = 0x04,
  CLRC663_IRQ0_ERR     = 0x02,
  CLRC663_IRQ0_RXSOF   = 0x01,
};

// VERSION register self-test value for this part: 0x18 (bits7-4 Version=0x1,
// bits3-0 SubVersion=0x8 — datasheet Table 242/243; the stock fw prints both).
static const uint8_t CLRC663_VERSION_CLRC663 = 0x18;

// TXCRCPRESET / RXCRCCON values for ISO14443A. 0x18 is the baseline (CRC disabled,
// used for REQA/WUPA and anticollision); bit0 enables CRC (used for SELECT + data).
static const uint8_t CLRC663_CRC_OFF = 0x18;
static const uint8_t CLRC663_CRC_ON  = 0x19;

// AN11022 recommended register block for ISO14443A 106 kbps (Miller/Manchester),
// written as a contiguous run starting at CLRC663_REG_DRVMOD (0x28). Antenna/analog
// tuning — these are the NXP reference defaults; expect to fine-tune on the bench.
static const uint8_t CLRC663_RECOM_14443A_106_BASE = CLRC663_REG_DRVMOD;
extern const uint8_t CLRC663_RECOM_14443A_106[18];

// ---- ISO/IEC 14443-3 Type A short-frame commands -------------------------------
enum {
  ISO14443A_CMD_REQA       = 0x26,  // 7-bit short frame
  ISO14443A_CMD_WUPA       = 0x52,  // 7-bit short frame
  ISO14443A_CMD_SEL_CL1    = 0x93,  // anticollision/select cascade level 1
  ISO14443A_CMD_SEL_CL2    = 0x95,  // cascade level 2 (7-byte UID)
  ISO14443A_CMD_SEL_CL3    = 0x97,  // cascade level 3 (10-byte UID)
  ISO14443A_CASCADE_TAG    = 0x88,  // CT — first byte of a CLn UID when more follows
  ISO14443A_CMD_HLTA       = 0x50,  // HALT (followed by 0x00 + CRC_A)
};

// ---- ISO14443A CRC_A (ISO/IEC 14443-3, MIFARE) ---------------------------------
// Computes the 16-bit CRC_A. Transmitted LSB first: low byte then high byte.
// Reference vector: HLTA payload {0x50,0x00} -> CRC_A 0xCD57 -> on-wire 0x57,0xCD.
uint16_t iso14443a_crc(const uint8_t *data, size_t len);

// Append the CRC_A of data[0..len) to out (out must hold len+2); returns len+2.
size_t iso14443a_append_crc(const uint8_t *data, size_t len, uint8_t *out, size_t out_cap);

// ---- UID helpers ---------------------------------------------------------------
// Block Check Character: XOR of the four UID bytes of a cascade level (ISO14443-3).
uint8_t iso14443a_bcc(const uint8_t uid_cl[4]);

// Format a UID as MSB-first upper-hex with no separators (matches the stock badge
// render, e.g. {0x67,0x8C,0x42,0x64} -> "678C4264"). Writes a NUL-terminated string;
// out must hold 2*len+1. Returns the string length (0 if len==0 or buffer too small).
size_t rfid_uid_to_hex(const uint8_t *uid, size_t len, char *out, size_t out_cap);

// Parse an upper/lower-hex UID string (no separators) into bytes. Returns the byte
// count (0 on malformed/odd-length/overflow). out_cap is the byte capacity.
size_t rfid_uid_from_hex(const char *hex, uint8_t *out, size_t out_cap);
