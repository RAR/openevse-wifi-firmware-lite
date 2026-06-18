#include "lite_clrc663.h"
#include <string.h>

// ---- register / FIFO plumbing --------------------------------------------------

uint8_t Clrc663::readReg(uint8_t reg)
{
  _io.cs(_io.ctx, true);
  _io.xfer(_io.ctx, clrc663_addr_read(reg));   // address byte (MISO don't-care here)
  uint8_t v = _io.xfer(_io.ctx, 0x00);         // value clocked back on the next byte
  _io.cs(_io.ctx, false);
  return v;
}

void Clrc663::writeReg(uint8_t reg, uint8_t val)
{
  _io.cs(_io.ctx, true);
  _io.xfer(_io.ctx, clrc663_addr_write(reg));
  _io.xfer(_io.ctx, val);
  _io.cs(_io.ctx, false);
}

void Clrc663::writeRegs(uint8_t start, const uint8_t *vals, uint8_t n)
{
  // One CS-low burst: address byte then n data bytes. The register address
  // auto-increments (start, start+1, …) — used for the recommended-register block.
  _io.cs(_io.ctx, true);
  _io.xfer(_io.ctx, clrc663_addr_write(start));
  for (uint8_t i = 0; i < n; ++i) _io.xfer(_io.ctx, vals[i]);
  _io.cs(_io.ctx, false);
}

void Clrc663::flushFifo()
{
  writeReg(CLRC663_REG_FIFOCONTROL, 0x10);     // FlushFIFO (bit4)
}

void Clrc663::writeFifo(const uint8_t *data, uint8_t n)
{
  // FIFODATA does not auto-increment — every byte after the address lands in the FIFO.
  _io.cs(_io.ctx, true);
  _io.xfer(_io.ctx, clrc663_addr_write(CLRC663_REG_FIFODATA));
  for (uint8_t i = 0; i < n; ++i) _io.xfer(_io.ctx, data[i]);
  _io.cs(_io.ctx, false);
}

uint8_t Clrc663::readFifo(uint8_t *out, uint8_t cap)
{
  uint8_t n = fifoLength();
  if (n > cap) n = cap;
  if (n == 0) return 0;
  // Burst read: send the FIFODATA read-address; data is delayed one byte, so the
  // value for slot i arrives on the (i+1)-th transfer. The final TX byte is 0x00.
  _io.cs(_io.ctx, true);
  _io.xfer(_io.ctx, clrc663_addr_read(CLRC663_REG_FIFODATA));   // primes; result discarded
  for (uint8_t i = 0; i < n; ++i) {
    uint8_t next = (uint8_t)((i + 1 < n) ? clrc663_addr_read(CLRC663_REG_FIFODATA) : 0x00);
    out[i] = _io.xfer(_io.ctx, next);
  }
  _io.cs(_io.ctx, false);
  return n;
}

// ---- lifecycle -----------------------------------------------------------------

void Clrc663::softReset()
{
  command(CLRC663_CMD_SOFTRESET);
  _io.delay_ms(_io.ctx, 10);                   // let the oscillator restart
  command(CLRC663_CMD_IDLE);
}

void Clrc663::init14443aReader()
{
  command(CLRC663_CMD_IDLE);
  flushFifo();

  // LoadProtocol consumes two bytes from the FIFO: TX index, RX index. 0/0 selects
  // ISO14443A 106 kbps (Miller/Manchester) for both directions.
  uint8_t proto[2] = {0x00, 0x00};
  writeFifo(proto, 2);
  command(CLRC663_CMD_LOADPROTOCOL);
  _io.delay_ms(_io.ctx, 10);
  flushFifo();

  // AN11022 recommended analog/timing block (DrvMode..RxAna). HW-tune on the bench.
  writeRegs(CLRC663_RECOM_14443A_106_BASE, CLRC663_RECOM_14443A_106, 18);

  // CRC baseline off (REQA/anticollision); transceive() flips it per frame.
  writeReg(CLRC663_REG_TXCRCPRESET, CLRC663_CRC_OFF);
  writeReg(CLRC663_REG_RXCRCCON,    CLRC663_CRC_OFF);

  // Clear any stale interrupt latches.
  clearIrq0();
}

// ---- ISO/IEC 14443-3 Type A ----------------------------------------------------

bool Clrc663::transceive(const uint8_t *tx, uint8_t tx_len, uint8_t tx_last_bits, bool crc,
                         uint8_t *rx, uint8_t rx_cap, uint8_t *rx_len)
{
  if (rx_len) *rx_len = 0;

  command(CLRC663_CMD_IDLE);
  flushFifo();

  writeReg(CLRC663_REG_TXCRCPRESET, crc ? CLRC663_CRC_ON : CLRC663_CRC_OFF);
  writeReg(CLRC663_REG_RXCRCCON,    crc ? CLRC663_CRC_ON : CLRC663_CRC_OFF);
  // TxDataNum: low 3 bits = valid bits in the last byte (0 = full byte); DataEn(0x08)
  // keeps the data stream enabled. 7-bit short frames (REQA/WUPA) pass tx_last_bits=7.
  writeReg(CLRC663_REG_TXDATANUM, (uint8_t)((tx_last_bits & 0x07) | 0x08));
  writeReg(CLRC663_REG_RXBITCTRL, 0x00);

  if (tx_len) writeFifo(tx, tx_len);
  clearIrq0();
  command(CLRC663_CMD_TRANSCEIVE);

  // Poll IRQ0 for completion (Rx) / Idle / Error. Bounded by _poll_ms (1 ms ticks).
  uint8_t irq = 0;
  for (uint16_t i = 0; i < _poll_ms; ++i) {
    irq = readReg(CLRC663_REG_IRQ0);
    if (irq & (CLRC663_IRQ0_RX | CLRC663_IRQ0_IDLE | CLRC663_IRQ0_ERR)) break;
    _io.delay_ms(_io.ctx, 1);
  }
  command(CLRC663_CMD_IDLE);

  if (irq & CLRC663_IRQ0_ERR) return false;
  if (!(irq & CLRC663_IRQ0_RX)) return false;   // timeout / no PICC answered

  uint8_t got = readFifo(rx, rx_cap);
  if (rx_len) *rx_len = got;
  return got > 0;
}

bool Clrc663::reqaWupa(uint8_t cmd, uint16_t *atqa)
{
  uint8_t tx = cmd;
  uint8_t rx[2] = {0};
  uint8_t rl = 0;
  // 7-bit short frame, no CRC.
  if (!transceive(&tx, 1, 7, false, rx, sizeof(rx), &rl) || rl < 2) return false;
  if (atqa) *atqa = (uint16_t)(rx[0] | ((uint16_t)rx[1] << 8));
  return true;
}

bool Clrc663::anticollideSelect(uint8_t sel_cmd, uint8_t cl_uid[4], uint8_t *sak, bool *cascade)
{
  // Anticollision: SEL + NVB=0x20 (only the 2 prefix bytes sent), no CRC.
  // Response = 4 UID/CT bytes + BCC.
  uint8_t ac[2] = { sel_cmd, 0x20 };
  uint8_t rx[5] = {0};
  uint8_t rl = 0;
  if (!transceive(ac, 2, 0, false, rx, sizeof(rx), &rl) || rl < 5) return false;
  for (int i = 0; i < 4; ++i) cl_uid[i] = rx[i];
  if (iso14443a_bcc(cl_uid) != rx[4]) return false;     // BCC integrity check

  // Select: SEL + NVB=0x70 + 4 UID + BCC, CRC on. Response = SAK (+CRC).
  uint8_t sel[7] = { sel_cmd, 0x70, cl_uid[0], cl_uid[1], cl_uid[2], cl_uid[3], rx[4] };
  uint8_t srx[3] = {0};
  uint8_t sl = 0;
  if (!transceive(sel, 7, 0, true, srx, sizeof(srx), &sl) || sl < 1) return false;
  if (sak)     *sak = srx[0];
  if (cascade) *cascade = (srx[0] & 0x04) != 0;          // SAK bit2 => UID incomplete
  return true;
}

bool Clrc663::activate(Iso14443aTag *tag)
{
  if (!tag) return false;
  memset(tag, 0, sizeof(*tag));

  uint16_t atqa = 0;
  if (!reqa(&atqa)) return false;
  tag->atqa = atqa;

  static const uint8_t sels[3] = {
    ISO14443A_CMD_SEL_CL1, ISO14443A_CMD_SEL_CL2, ISO14443A_CMD_SEL_CL3,
  };
  uint8_t idx = 0;
  for (int lvl = 0; lvl < 3; ++lvl) {
    uint8_t cl[4] = {0};
    uint8_t sak = 0;
    bool cascade = false;
    if (!anticollideSelect(sels[lvl], cl, &sak, &cascade)) return false;

    if (cascade) {
      // cl[0] is the Cascade Tag (0x88); the next 3 bytes are UID. Continue to CLn+1.
      if (idx + 3 > (uint8_t)sizeof(tag->uid)) return false;
      tag->uid[idx++] = cl[1];
      tag->uid[idx++] = cl[2];
      tag->uid[idx++] = cl[3];
    } else {
      if (idx + 4 > (uint8_t)sizeof(tag->uid)) return false;
      for (int i = 0; i < 4; ++i) tag->uid[idx++] = cl[i];
      tag->sak     = sak;
      tag->uid_len = idx;
      return true;
    }
  }
  return false;   // cascade never terminated (malformed)
}
