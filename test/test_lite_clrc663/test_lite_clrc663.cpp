#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../src/lite/lite_clrc663.h"
#include <string.h>

// ---- Scripted-slave mock of a CLRC663 over the SPI xfer interface --------------
// Emulates just enough of the chip for the driver's code paths: register array with
// write auto-increment, a TX-capture FIFO, IRQ0 set/clear semantics, and a queue of
// canned RX responses popped one per Transceive command.
struct MockClrc663 {
  uint8_t reg[128];

  // per-CS-transaction state
  bool    in_txn;
  int     xfer_idx;
  uint8_t cur_reg;
  bool    cur_read;
  int     wr_offset;       // register write auto-increment

  uint8_t tx_capture[64];  // bytes the driver wrote to FIFODATA (the TX frame)
  uint8_t tx_capture_len;

  uint8_t staged[16];      // current RX response in the FIFO
  uint8_t staged_len;
  uint8_t staged_rd;

  uint8_t resp[6][16];     // scripted RX responses, popped per Transceive
  uint8_t resp_len[6];
  int     n_resp;
  int     resp_idx;

  uint32_t delay_total;
};

static void mock_reset(MockClrc663 &m) {
  memset(&m, 0, sizeof(m));
  m.reg[CLRC663_REG_VERSION] = CLRC663_VERSION_CLRC663;  // healthy reader
}

static void mock_push_resp(MockClrc663 &m, const uint8_t *bytes, uint8_t n) {
  memcpy(m.resp[m.n_resp], bytes, n);
  m.resp_len[m.n_resp] = n;
  m.n_resp++;
}

static void mock_command(MockClrc663 &m, uint8_t cmd) {
  if (cmd == CLRC663_CMD_TRANSCEIVE) {
    if (m.resp_idx < m.n_resp) {
      m.staged_len = m.resp_len[m.resp_idx];
      memcpy(m.staged, m.resp[m.resp_idx], m.staged_len);
      m.staged_rd = 0;
      m.reg[CLRC663_REG_FIFOLENGTH] = m.staged_len;
      m.reg[CLRC663_REG_IRQ0] |= CLRC663_IRQ0_RX;
      m.resp_idx++;
    } else {
      m.staged_len = 0;
      m.reg[CLRC663_REG_FIFOLENGTH] = 0;
      m.reg[CLRC663_REG_IRQ0] |= CLRC663_IRQ0_IDLE;   // no card -> timeout-ish
    }
  } else if (cmd == CLRC663_CMD_LOADPROTOCOL) {
    m.tx_capture_len = 0;   // consumed
  }
}

static uint8_t mock_xfer(void *ctx, uint8_t out) {
  MockClrc663 &m = *(MockClrc663 *)ctx;
  if (m.xfer_idx == 0) {                 // address byte
    m.cur_reg   = (uint8_t)(out >> 1);
    m.cur_read  = (out & 1) != 0;
    m.wr_offset = 0;
    m.xfer_idx++;
    return 0;
  }
  uint8_t ret = 0;
  if (m.cur_read) {
    if (m.cur_reg == CLRC663_REG_FIFODATA)
      ret = (m.staged_rd < m.staged_len) ? m.staged[m.staged_rd++] : 0;
    else
      ret = m.reg[m.cur_reg];
  } else {
    if (m.cur_reg == CLRC663_REG_FIFODATA) {
      if (m.tx_capture_len < sizeof(m.tx_capture)) m.tx_capture[m.tx_capture_len++] = out;
    } else if (m.cur_reg == CLRC663_REG_COMMAND) {
      m.reg[CLRC663_REG_COMMAND] = out;
      mock_command(m, out);
    } else if (m.cur_reg == CLRC663_REG_FIFOCONTROL) {
      if (out & 0x10) { m.tx_capture_len = 0; m.staged_len = 0; m.staged_rd = 0; }
      m.reg[CLRC663_REG_FIFOCONTROL] = out;
    } else if (m.cur_reg == CLRC663_REG_IRQ0) {
      if (out & 0x80) m.reg[CLRC663_REG_IRQ0] |= (out & 0x7F);   // set
      else            m.reg[CLRC663_REG_IRQ0] &= ~(out & 0x7F);  // clear
    } else {
      uint8_t a = (uint8_t)(m.cur_reg + m.wr_offset);            // auto-increment
      if (a < 128) m.reg[a] = out;
      m.wr_offset++;
    }
  }
  m.xfer_idx++;
  return ret;
}

static void mock_cs(void *ctx, bool assert) {
  MockClrc663 &m = *(MockClrc663 *)ctx;
  if (assert) { m.in_txn = true; m.xfer_idx = 0; }
  else        { m.in_txn = false; }
}

static void mock_delay(void *ctx, uint32_t ms) {
  ((MockClrc663 *)ctx)->delay_total += ms;
}

static Clrc663Io mock_io(MockClrc663 &m) {
  Clrc663Io io;
  io.xfer = mock_xfer; io.cs = mock_cs; io.delay_ms = mock_delay; io.ctx = &m;
  return io;
}

// ---- plumbing ------------------------------------------------------------------

TEST_CASE("readReg / writeReg round-trip via the address byte") {
  MockClrc663 m; mock_reset(m);
  Clrc663 dev(mock_io(m));

  dev.writeReg(CLRC663_REG_TXDATANUM, 0x0F);
  CHECK(m.reg[CLRC663_REG_TXDATANUM] == 0x0F);
  CHECK(dev.readReg(CLRC663_REG_TXDATANUM) == 0x0F);
}

TEST_CASE("version / selfTest read the VERSION register") {
  MockClrc663 m; mock_reset(m);
  Clrc663 dev(mock_io(m));
  CHECK(dev.version() == 0x18);
  CHECK(dev.selfTest());

  m.reg[CLRC663_REG_VERSION] = 0x00;   // dead/absent reader
  CHECK_FALSE(dev.selfTest());
}

TEST_CASE("writeRegs writes a contiguous auto-incrementing block") {
  MockClrc663 m; mock_reset(m);
  Clrc663 dev(mock_io(m));
  dev.writeRegs(CLRC663_RECOM_14443A_106_BASE, CLRC663_RECOM_14443A_106, 18);
  for (int i = 0; i < 18; ++i)
    CHECK(m.reg[CLRC663_RECOM_14443A_106_BASE + i] == CLRC663_RECOM_14443A_106[i]);
}

TEST_CASE("writeFifo captures the TX frame; flushFifo clears it") {
  MockClrc663 m; mock_reset(m);
  Clrc663 dev(mock_io(m));
  const uint8_t frame[3] = {0x93, 0x20, 0xAB};
  dev.writeFifo(frame, 3);
  REQUIRE(m.tx_capture_len == 3);
  CHECK(m.tx_capture[0] == 0x93);
  CHECK(m.tx_capture[2] == 0xAB);
  dev.flushFifo();
  CHECK(m.tx_capture_len == 0);
}

// ---- transceive ----------------------------------------------------------------

TEST_CASE("transceive sends the frame and returns the scripted response") {
  MockClrc663 m; mock_reset(m);
  const uint8_t atqa[2] = {0x04, 0x00};
  mock_push_resp(m, atqa, 2);
  Clrc663 dev(mock_io(m));

  const uint8_t reqa = ISO14443A_CMD_REQA;
  uint8_t rx[4] = {0}; uint8_t rl = 0;
  CHECK(dev.transceive(&reqa, 1, 7, false, rx, sizeof(rx), &rl));
  CHECK(rl == 2);
  CHECK(rx[0] == 0x04);
  CHECK(m.tx_capture[0] == ISO14443A_CMD_REQA);            // the byte we sent
  CHECK(m.reg[CLRC663_REG_TXDATANUM] == (0x07 | 0x08));    // 7 valid bits + DataEn
  CHECK(m.reg[CLRC663_REG_TXCRCPRESET] == CLRC663_CRC_OFF);
}

TEST_CASE("transceive reports no-card when nothing answers") {
  MockClrc663 m; mock_reset(m);          // no scripted responses
  Clrc663 dev(mock_io(m));
  dev.setPollTimeoutMs(3);               // keep the poll loop short
  const uint8_t reqa = ISO14443A_CMD_REQA;
  uint8_t rx[4] = {0}; uint8_t rl = 0;
  CHECK_FALSE(dev.transceive(&reqa, 1, 7, false, rx, sizeof(rx), &rl));
  CHECK(rl == 0);
}

// ---- full activation -----------------------------------------------------------

TEST_CASE("activate reads a 4-byte ISO14443A UID end-to-end") {
  MockClrc663 m; mock_reset(m);
  // REQA -> ATQA
  const uint8_t atqa[2]  = {0x04, 0x00};
  // SEL CL1 anticollision -> 4 UID + BCC (0x67^0x8C^0x42^0x64 = 0xCD)
  const uint8_t cl1[5]   = {0x67, 0x8C, 0x42, 0x64, 0xCD};
  // SELECT CL1 -> SAK (bit2 clear => UID complete) + CRC
  const uint8_t sak[3]   = {0x08, 0x00, 0x00};
  mock_push_resp(m, atqa, 2);
  mock_push_resp(m, cl1, 5);
  mock_push_resp(m, sak, 3);

  Clrc663 dev(mock_io(m));
  Iso14443aTag tag;
  REQUIRE(dev.activate(&tag));
  CHECK(tag.uid_len == 4);
  CHECK(tag.uid[0] == 0x67);
  CHECK(tag.uid[1] == 0x8C);
  CHECK(tag.uid[2] == 0x42);
  CHECK(tag.uid[3] == 0x64);
  CHECK(tag.sak == 0x08);
  CHECK(tag.atqa == 0x0004);

  char hex[16];
  rfid_uid_to_hex(tag.uid, tag.uid_len, hex, sizeof(hex));
  CHECK(strcmp(hex, "678C4264") == 0);    // the stock badge render
}

TEST_CASE("activate rejects a UID whose BCC is wrong") {
  MockClrc663 m; mock_reset(m);
  const uint8_t atqa[2]   = {0x04, 0x00};
  const uint8_t bad_cl[5] = {0x67, 0x8C, 0x42, 0x64, 0x00};  // BCC should be 0xCD
  mock_push_resp(m, atqa, 2);
  mock_push_resp(m, bad_cl, 5);
  Clrc663 dev(mock_io(m));
  Iso14443aTag tag;
  CHECK_FALSE(dev.activate(&tag));
}
