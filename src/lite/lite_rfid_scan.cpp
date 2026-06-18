#ifdef OPENEVSE_LITE
#ifdef LITE_RFID_SCAN
#include "lite_rfid_scan.h"
#include "lite_clrc663.h"
#include "lite_rfid_proto.h"

#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>
#include "em_cmu.h"
#include "em_gpio.h"

// Results + a human-readable event log, both in RAM so they can be read back over
// SWD (J-Link/openocd) after boot — no WiFi/console needed.
volatile LiteRfidScanResult g_rfid_scan_result = {0};
char                        g_rfid_scan_log[1400] = {0};
volatile uint16_t           g_rfid_scan_log_len   = 0;

namespace {

constexpr uint8_t HALF_US = 2;   // ~250 kHz bit-bang; conservative for bring-up

char port_letter(uint8_t p) { return (char)('A' + p); }

void scan_logf(const char *fmt, ...)
{
  if (g_rfid_scan_log_len > sizeof(g_rfid_scan_log) - 96) return;
  va_list ap; va_start(ap, fmt);
  int n = vsnprintf(g_rfid_scan_log + g_rfid_scan_log_len,
                    sizeof(g_rfid_scan_log) - g_rfid_scan_log_len, fmt, ap);
  va_end(ap);
  if (n > 0) {
    g_rfid_scan_log_len += n;
    if (g_rfid_scan_log_len < sizeof(g_rfid_scan_log) - 1)
      g_rfid_scan_log[g_rfid_scan_log_len++] = '\n';
    g_rfid_scan_log[g_rfid_scan_log_len] = 0;
  }
}

// ---- SPI (bit-bang, mode 0) -------------------------------------------------
struct ScanIo {
  GPIO_Port_TypeDef sclk_port; uint8_t sclk_pin;
  GPIO_Port_TypeDef mosi_port; uint8_t mosi_pin;
  GPIO_Port_TypeDef miso_port; uint8_t miso_pin;
  GPIO_Port_TypeDef cs_port;   uint8_t cs_pin;
};

uint8_t io_xfer(void *ctx, uint8_t out)
{
  ScanIo *s = static_cast<ScanIo *>(ctx);
  uint8_t in = 0;
  for (int i = 7; i >= 0; --i) {
    if (out & (1 << i)) GPIO_PinOutSet(s->mosi_port, s->mosi_pin);
    else                GPIO_PinOutClear(s->mosi_port, s->mosi_pin);
    delayMicroseconds(HALF_US);
    GPIO_PinOutSet(s->sclk_port, s->sclk_pin);
    if (GPIO_PinInGet(s->miso_port, s->miso_pin)) in |= (1 << i);
    delayMicroseconds(HALF_US);
    GPIO_PinOutClear(s->sclk_port, s->sclk_pin);
  }
  return in;
}
void io_cs(void *ctx, bool assert)
{
  ScanIo *s = static_cast<ScanIo *>(ctx);
  if (assert) GPIO_PinOutClear(s->cs_port, s->cs_pin);
  else        GPIO_PinOutSet(s->cs_port, s->cs_pin);
}
void io_delay(void *, uint32_t ms) { delay(ms); }

bool excluded(uint8_t port, uint8_t pin)   // pins that would break the session
{
  if (port == gpioPortF && (pin <= 3 || pin == 11)) return true; // SWD/JTAG + RESET
  if (port == gpioPortE && (pin == 6 || pin == 7))  return true; // ATmega UART
  return false;
}

struct PinId { GPIO_Port_TypeDef port; uint8_t pin; };
// Confirmed reader↔WGM lines (bench continuity): PB4 (~40k pull) + PF10 (~46k pull) = MISO/CS
// (idle-high); PE5 (~330Ω series) = a driven signal (SCLK/MOSI). The 4th driven line isn't mapped
// yet, so SCLK/MOSI sweep all pins to catch it; CS/MISO stay on the confirmed pulled/series lines.
const PinId CS_CANDS[]   = { {gpioPortF, 10}, {gpioPortB, 4} };
const PinId MISO_CANDS[] = { {gpioPortB, 4}, {gpioPortF, 10}, {gpioPortE, 5} };

// ---- I2C (bit-bang, open-drain) --------------------------------------------
struct I2cBus { GPIO_Port_TypeDef scl_p; uint8_t scl_n; GPIO_Port_TypeDef sda_p; uint8_t sda_n; };
inline void i2c_dly()          { delayMicroseconds(5); }
inline void scl1(I2cBus &b)    { GPIO_PinOutSet(b.scl_p, b.scl_n); }    // release (pull-up high)
inline void scl0(I2cBus &b)    { GPIO_PinOutClear(b.scl_p, b.scl_n); }  // drive low
inline void sda1(I2cBus &b)    { GPIO_PinOutSet(b.sda_p, b.sda_n); }
inline void sda0(I2cBus &b)    { GPIO_PinOutClear(b.sda_p, b.sda_n); }
inline int  sdar(I2cBus &b)    { return GPIO_PinInGet(b.sda_p, b.sda_n); }

void i2c_init(I2cBus &b)
{
  // Open-drain WITH internal pull-up: PB4/PC5 showed as input-pull-up in the stock
  // map (no external pull-ups), so the lines idle high only if we enable the pull.
  GPIO_PinModeSet(b.scl_p, b.scl_n, gpioModeWiredAndPullUp, 1);   // released = high
  GPIO_PinModeSet(b.sda_p, b.sda_n, gpioModeWiredAndPullUp, 1);
}
void i2c_start(I2cBus &b) { sda1(b); scl1(b); i2c_dly(); sda0(b); i2c_dly(); scl0(b); i2c_dly(); }
void i2c_stop(I2cBus &b)  { sda0(b); i2c_dly(); scl1(b); i2c_dly(); sda1(b); i2c_dly(); }

bool i2c_wr(I2cBus &b, uint8_t v)   // returns true if slave ACKed
{
  for (int i = 7; i >= 0; --i) {
    if (v & (1 << i)) sda1(b); else sda0(b);
    i2c_dly(); scl1(b); i2c_dly(); scl0(b); i2c_dly();
  }
  sda1(b);                                  // release for ACK
  i2c_dly(); scl1(b); i2c_dly();
  int ack = !sdar(b);                       // slave pulls low to ACK
  scl0(b); i2c_dly();
  return ack;
}
uint8_t i2c_rd(I2cBus &b, bool ack)
{
  uint8_t v = 0; sda1(b);
  for (int i = 7; i >= 0; --i) {
    scl1(b); i2c_dly(); if (sdar(b)) v |= (1 << i); scl0(b); i2c_dly();
  }
  if (ack) sda0(b); else sda1(b);
  i2c_dly(); scl1(b); i2c_dly(); scl0(b); sda1(b); i2c_dly();
  return v;
}
// Read one register. Returns true if the address ACKed (device present); *out=value.
bool i2c_read_reg(I2cBus &b, uint8_t addr7, uint8_t reg, uint8_t *out)
{
  i2c_start(b);
  if (!i2c_wr(b, (uint8_t)(addr7 << 1))) { i2c_stop(b); return false; }
  i2c_wr(b, reg);
  i2c_start(b);                              // repeated start
  if (!i2c_wr(b, (uint8_t)((addr7 << 1) | 1))) { i2c_stop(b); return false; }
  *out = i2c_rd(b, false);
  i2c_stop(b);
  return true;
}

// ---- UART (bit-bang, 8N1) ---------------------------------------------------
struct UartLine { GPIO_Port_TypeDef tx_p; uint8_t tx_n; GPIO_Port_TypeDef rx_p; uint8_t rx_n; uint32_t bit_us; };

void uart_init(UartLine &u)
{
  GPIO_PinModeSet(u.tx_p, u.tx_n, gpioModePushPull, 1);       // TX idle high
  GPIO_PinModeSet(u.rx_p, u.rx_n, gpioModeInputPull, 1);      // RX input, pull-up (idle high)
}
void uart_send(UartLine &u, uint8_t b)
{
  GPIO_PinOutClear(u.tx_p, u.tx_n); delayMicroseconds(u.bit_us);          // start
  for (int i = 0; i < 8; ++i) {
    if (b & (1 << i)) GPIO_PinOutSet(u.tx_p, u.tx_n);
    else              GPIO_PinOutClear(u.tx_p, u.tx_n);
    delayMicroseconds(u.bit_us);
  }
  GPIO_PinOutSet(u.tx_p, u.tx_n); delayMicroseconds(u.bit_us);            // stop
}
// Wait (up to timeout_us) for a start bit on RX, then sample one 8N1 byte.
bool uart_recv(UartLine &u, uint8_t *out, uint32_t timeout_us)
{
  uint32_t spins = timeout_us;                  // ~1us granularity busy-poll
  while (GPIO_PinInGet(u.rx_p, u.rx_n)) { if (spins-- == 0) return false; delayMicroseconds(1); }
  delayMicroseconds(u.bit_us + u.bit_us / 2);   // skip to middle of bit0
  uint8_t v = 0;
  for (int i = 0; i < 8; ++i) {
    if (GPIO_PinInGet(u.rx_p, u.rx_n)) v |= (1 << i);
    delayMicroseconds(u.bit_us);
  }
  return (out ? (*out = v, true) : true);
}

} // namespace

void lite_rfid_scan_run()
{
  CMU_ClockEnable(cmuClock_GPIO, true);
  scan_logf("rfid-scan start");

  // TEST: let the ATmega RUN (PF11 high = reset deasserted) to find out whether it actually
  // contends on the reader bus. If the WGM still reads VERSION==0x18 with the ATmega running,
  // the ATmega doesn't drive the reader → no arbitration needed → lite can read it freely.
  GPIO_PinModeSet(gpioPortF, 11, gpioModePushPull, 1);
  delay(200);   // let the ATmega boot
  scan_logf("ATmega RUNNING (PF11 high) - contention test");

  // Drive every non-excluded GPIO HIGH first. Stock keeps several outputs HIGH while the
  // reader runs (its enable / active-low-reset idles high), and our prior hi-Z left those
  // floating — likely powering the reader down for the whole scan. Driving high asserts any
  // such enable; each pass below overrides only the pins it tests (SCLK gets driven LOW per
  // combo regardless), so this just guarantees the reader stays awake. (PF11=ATmega-reset and
  // SWD/UART are excluded, so they're untouched here.)
  for (uint8_t p = gpioPortA; p <= gpioPortF; ++p)
    for (uint8_t n = 0; n < 16; ++n)
      if (!excluded(p, n)) GPIO_PinModeSet((GPIO_Port_TypeDef)p, n, gpioModePushPull, 1);
  delay(20);

  bool done = false;

  // ===================== I2C pass (the two pull-up pins) — try first =========
  {
    const I2cBus orders[2] = {
      { gpioPortB, 4, gpioPortF, 10 },   // SCL=PB4, SDA=PF10  (confirmed reader↔WGM pulls)
      { gpioPortF, 10, gpioPortB, 4 },   // SCL=PF10, SDA=PB4
    };
    uint16_t acks = 0;
    for (int o = 0; o < 2 && !done; ++o) {
      I2cBus b = orders[o];
      i2c_init(b);
      delay(2);
      i2c_start(b); i2c_stop(b);
      for (uint8_t addr = 0x08; addr <= 0x77 && !done; ++addr) {
        uint8_t v = 0;
        if (i2c_read_reg(b, addr, CLRC663_REG_VERSION, &v)) {
          ++acks;
          if (v != 0x00)   // suppress floating-bus false ACKs (SDA stuck low -> 0x00)
            scan_logf("I2C ACK @0x%02X SCL=%c%u SDA=%c%u VERSION=0x%02X", addr,
                      port_letter(b.scl_p), b.scl_n, port_letter(b.sda_p), b.sda_n, v);
          if (v == CLRC663_VERSION_CLRC663) {
            g_rfid_scan_result.found     = 1;
            g_rfid_scan_result.iface     = 2;
            g_rfid_scan_result.version   = v;
            g_rfid_scan_result.i2c_addr  = addr;
            g_rfid_scan_result.sclk_port = b.scl_p; g_rfid_scan_result.sclk_pin = b.scl_n; // SCL
            g_rfid_scan_result.mosi_port = b.sda_p; g_rfid_scan_result.mosi_pin = b.sda_n; // SDA
            scan_logf("I2C HIT addr=0x%02X SCL=%c%u SDA=%c%u", addr,
                      port_letter(b.scl_p), b.scl_n, port_letter(b.sda_p), b.sda_n);
            done = true;
          }
        }
      }
    }
    g_rfid_scan_result.i2c_acks = acks;
    scan_logf("I2C pass: acks=%u", acks);
  }

  // ===================== UART pass (bit-bang 8N1; baud + pin sweep) ==========
  if (!done) {
    const PinId  rxc[]   = { {gpioPortC, 5}, {gpioPortB, 4}, {gpioPortA, 1} };
    const uint32_t bauds[] = { 9600, 115200 };
    const uint8_t  reqs[]  = { 0x7F, 0xFF };   // VERSION read, two encodings
    uint16_t frames = 0;
    for (const PinId &rx : rxc) {
      if (done) break;
      for (uint8_t txp = gpioPortA; txp <= gpioPortF && !done; ++txp) {
        for (uint8_t txn = 0; txn < 16 && !done; ++txn) {
          if (excluded(txp, txn)) continue;
          if (txp == rx.port && txn == rx.pin) continue;
          for (uint32_t baud : bauds) {
            if (done) break;
            UartLine u{ (GPIO_Port_TypeDef)txp, txn, rx.port, rx.pin, 1000000UL / baud };
            uart_init(u);
            delayMicroseconds(200);
            for (uint8_t req : reqs) {
              uart_send(u, req);
              uint8_t resp = 0;
              if (uart_recv(u, &resp, u.bit_us * 12)) {
                ++frames;
                if (resp == CLRC663_VERSION_CLRC663) {
                  g_rfid_scan_result.found     = 1;
                  g_rfid_scan_result.iface     = 3;
                  g_rfid_scan_result.version   = resp;
                  g_rfid_scan_result.baud      = baud;
                  g_rfid_scan_result.sclk_port = txp; g_rfid_scan_result.sclk_pin = txn;        // host TX
                  g_rfid_scan_result.miso_port = rx.port; g_rfid_scan_result.miso_pin = rx.pin; // host RX
                  scan_logf("UART HIT v=0x%02X TX=%c%u RX=%c%u baud=%lu req=0x%02X",
                            resp, port_letter(txp), txn, port_letter(rx.port), rx.pin,
                            (unsigned long)baud, req);
                  done = true; break;
                }
              }
            }
          }
        }
      }
    }
    g_rfid_scan_result.uart_frames = frames;
    scan_logf("UART pass: framed-replies=%u", frames);
  }

  // ===================== SPI pass (only if I2C/UART missed) ==================
  if (!done) {
  ScanIo s;
  Clrc663Io io{ io_xfer, io_cs, io_delay, &s };
  Clrc663 reader(io);
  uint32_t tried = 0, plausible = 0;

  for (const PinId &cs : CS_CANDS) {
    if (done) break;
    s.cs_port = cs.port; s.cs_pin = cs.pin;
    GPIO_PinModeSet(cs.port, cs.pin, gpioModePushPull, 1);
    for (const PinId &mi : MISO_CANDS) {
      if (done) break;
      if (mi.port == cs.port && mi.pin == cs.pin) continue;   // CS != MISO
      s.miso_port = mi.port; s.miso_pin = mi.pin;
      GPIO_PinModeSet(mi.port, mi.pin, gpioModeInput, 0);
      for (uint8_t scp = gpioPortA; scp <= gpioPortF && !done; ++scp) {
        for (uint8_t scn = 0; scn < 16 && !done; ++scn) {
          if (excluded(scp, scn)) continue;
          if (scp == cs.port && scn == cs.pin) continue;
          if (scp == mi.port && scn == mi.pin) continue;
          s.sclk_port = (GPIO_Port_TypeDef)scp; s.sclk_pin = scn;
          GPIO_PinModeSet((GPIO_Port_TypeDef)scp, scn, gpioModePushPull, 0);
          for (uint8_t mop = gpioPortA; mop <= gpioPortF && !done; ++mop) {
            for (uint8_t mon = 0; mon < 16 && !done; ++mon) {
              if (excluded(mop, mon)) continue;
              if (mop == scp && mon == scn) continue;
              if (mop == cs.port && mon == cs.pin) continue;
              if (mop == mi.port && mon == mi.pin) continue;
              s.mosi_port = (GPIO_Port_TypeDef)mop; s.mosi_pin = mon;
              GPIO_PinModeSet((GPIO_Port_TypeDef)mop, mon, gpioModePushPull, 0);

              uint8_t v = reader.version();
              ++tried;
              if (v != 0x00 && v != 0xFF) ++plausible;
              if (v == CLRC663_VERSION_CLRC663) {
                g_rfid_scan_result.found     = 1;
                g_rfid_scan_result.iface     = 1;
                g_rfid_scan_result.version   = v;
                g_rfid_scan_result.sclk_port = scp; g_rfid_scan_result.sclk_pin = scn;
                g_rfid_scan_result.mosi_port = mop; g_rfid_scan_result.mosi_pin = mon;
                g_rfid_scan_result.miso_port = mi.port; g_rfid_scan_result.miso_pin = mi.pin;
                g_rfid_scan_result.cs_port   = cs.port; g_rfid_scan_result.cs_pin   = cs.pin;
                scan_logf("SPI HIT v=0x%02X SCLK=%c%u MOSI=%c%u MISO=%c%u CS=%c%u", v,
                          port_letter(scp), scn, port_letter(mop), mon,
                          port_letter(mi.port), mi.pin, port_letter(cs.port), cs.pin);
                done = true;
              }
            }
          }
        }
      }
    }
  }
  g_rfid_scan_result.combos_tried = (uint16_t)tried;
  g_rfid_scan_result.plausible    = (uint16_t)plausible;
  scan_logf("SPI pass: tried=%lu found=%u plausible=%lu",
            (unsigned long)tried, g_rfid_scan_result.found, (unsigned long)plausible);
  }  // end SPI pass block

  if (!g_rfid_scan_result.found) scan_logf("NO READER FOUND on SPI/I2C/UART candidates");

  // Return swept pins to hi-Z so normal boot starts clean.
  for (const PinId &cs : CS_CANDS)   GPIO_PinModeSet(cs.port, cs.pin, gpioModeDisabled, 0);
  for (const PinId &mi : MISO_CANDS) GPIO_PinModeSet(mi.port, mi.pin, gpioModeDisabled, 0);
  for (uint8_t p = gpioPortA; p <= gpioPortF; ++p)
    for (uint8_t n = 0; n < 16; ++n)
      if (!excluded(p, n)) GPIO_PinModeSet((GPIO_Port_TypeDef)p, n, gpioModeDisabled, 0);

  g_rfid_scan_result.magic = LITE_RFID_SCAN_MAGIC;   // last: signals completion
  scan_logf("rfid-scan done");
}

#endif // LITE_RFID_SCAN
#endif // OPENEVSE_LITE
