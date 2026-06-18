#pragma once
#include <stddef.h>
#include <stdint.h>
#include "lite_rfid_proto.h"

// CLRC663 reader driver. Pure logic over an injected hardware-access interface
// (Clrc663Io) so it builds + runs in the native test env against a scripted-slave
// mock; the device build (lite_rfid.cpp) wires the IO hooks to the bit-banged SPI
// on the WGM160P GPIOs. The command/FIFO/transceive flow follows the public NXP
// MFRC630 reference (the CLRC663 is the same register/command family).

// SPI byte transfer is mode-0, MSB-first. cs(assert=true) drives the active-low
// chip-select LOW (selected). delay_ms paces reset/poll loops.
struct Clrc663Io {
  uint8_t (*xfer)(void *ctx, uint8_t out);
  void    (*cs)(void *ctx, bool assert);
  void    (*delay_ms)(void *ctx, uint32_t ms);
  void   *ctx;
};

struct Iso14443aTag {
  uint8_t  uid[10];
  uint8_t  uid_len;   // 4, 7, or 10 (0 = none)
  uint8_t  sak;
  uint16_t atqa;
};

class Clrc663 {
public:
  explicit Clrc663(const Clrc663Io &io) : _io(io) {}

  // --- register / FIFO plumbing -------------------------------------------------
  uint8_t readReg(uint8_t reg);
  void    writeReg(uint8_t reg, uint8_t val);
  void    writeRegs(uint8_t start, const uint8_t *vals, uint8_t n);
  void    command(uint8_t cmd) { writeReg(CLRC663_REG_COMMAND, cmd); }
  void    flushFifo();
  uint8_t fifoLength()       { return readReg(CLRC663_REG_FIFOLENGTH); }
  void    writeFifo(const uint8_t *data, uint8_t n);
  uint8_t readFifo(uint8_t *out, uint8_t cap);
  void    clearIrq0()        { writeReg(CLRC663_REG_IRQ0, 0x7F); } // bit7=0 => clear

  // --- lifecycle ----------------------------------------------------------------
  void    softReset();
  uint8_t version()  { return readReg(CLRC663_REG_VERSION); }
  bool    selfTest() { return version() == CLRC663_VERSION_CLRC663; }
  void    init14443aReader();

  // --- ISO/IEC 14443-3 Type A ---------------------------------------------------
  // Transceive one frame. tx_last_bits (0..7): bits valid in the last TX byte
  // (0 = whole byte). crc selects per-frame CRC. Returns true on a clean RX;
  // *rx_len gets the byte count copied into rx (capped at rx_cap).
  bool transceive(const uint8_t *tx, uint8_t tx_len, uint8_t tx_last_bits, bool crc,
                  uint8_t *rx, uint8_t rx_cap, uint8_t *rx_len);

  bool reqa(uint16_t *atqa) { return reqaWupa(ISO14443A_CMD_REQA, atqa); }
  bool wupa(uint16_t *atqa) { return reqaWupa(ISO14443A_CMD_WUPA, atqa); }

  // Full activation: REQA + anticollision/select cascade. Fills tag and returns
  // true if a single PICC was activated (uid/uid_len/sak/atqa populated).
  bool activate(Iso14443aTag *tag);

  // Max poll iterations transceive() waits for an RX/ERR/IDLE before giving up.
  // Each iteration is a 1 ms delay — generous default, overridable for tests.
  void setPollTimeoutMs(uint16_t ms) { _poll_ms = ms; }

private:
  bool reqaWupa(uint8_t cmd, uint16_t *atqa);
  bool anticollideSelect(uint8_t sel_cmd, uint8_t cl_uid[4], uint8_t *sak, bool *cascade);

  Clrc663Io _io;
  uint16_t  _poll_ms = 50;
};
