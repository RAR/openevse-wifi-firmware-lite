#include "lite_rfid_proto.h"

// AN11022 ISO14443A 106 kbps recommended register block (DrvMode..RxAna, regs
// 0x28..0x39). Values verbatim from the NXP reference / iwanders mfrc630 driver.
const uint8_t CLRC663_RECOM_14443A_106[18] = {
  0x8A, 0x08, 0x21, 0x1A, 0x18, 0x18, 0x0F, 0x27, 0x00,
  0xC0, 0x12, 0xCF, 0x00, 0x04, 0x90, 0x32, 0x12, 0x0A,
};

// ISO/IEC 14443-3 Type A CRC_A. Init 0x6363; the per-byte update is the standard
// MIFARE/ISO14443A polynomial (CRC-16/CCITT variant, reflected form). Verified
// against the canonical HLTA vector ({0x50,0x00} -> 0xCD57) in the unit tests.
uint16_t iso14443a_crc(const uint8_t *data, size_t len)
{
  uint16_t crc = 0x6363;
  for (size_t i = 0; i < len; ++i) {
    uint8_t b = (uint8_t)(data[i] ^ (crc & 0xFF));
    b = (uint8_t)(b ^ (b << 4));
    crc = (uint16_t)((crc >> 8) ^ ((uint16_t)b << 8) ^ ((uint16_t)b << 3) ^ ((uint16_t)b >> 4));
  }
  return crc;
}

size_t iso14443a_append_crc(const uint8_t *data, size_t len, uint8_t *out, size_t out_cap)
{
  if (out_cap < len + 2) return 0;
  for (size_t i = 0; i < len; ++i) out[i] = data[i];
  uint16_t crc = iso14443a_crc(data, len);
  out[len]     = (uint8_t)(crc & 0xFF);          // CRC_A is sent LSB first
  out[len + 1] = (uint8_t)((crc >> 8) & 0xFF);
  return len + 2;
}

uint8_t iso14443a_bcc(const uint8_t uid_cl[4])
{
  return (uint8_t)(uid_cl[0] ^ uid_cl[1] ^ uid_cl[2] ^ uid_cl[3]);
}

static char hex_nibble(uint8_t n) { return (char)(n < 10 ? '0' + n : 'A' + (n - 10)); }

size_t rfid_uid_to_hex(const uint8_t *uid, size_t len, char *out, size_t out_cap)
{
  if (len == 0 || out_cap < (2 * len + 1)) { if (out_cap) out[0] = '\0'; return 0; }
  size_t j = 0;
  for (size_t i = 0; i < len; ++i) {
    out[j++] = hex_nibble((uint8_t)(uid[i] >> 4));
    out[j++] = hex_nibble((uint8_t)(uid[i] & 0x0F));
  }
  out[j] = '\0';
  return j;
}

static int hex_val(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

size_t rfid_uid_from_hex(const char *hex, uint8_t *out, size_t out_cap)
{
  if (!hex) return 0;
  size_t n = 0;
  while (hex[0] && hex[1]) {
    int hi = hex_val(hex[0]);
    int lo = hex_val(hex[1]);
    if (hi < 0 || lo < 0) return 0;          // malformed
    if (n >= out_cap) return 0;              // overflow
    out[n++] = (uint8_t)((hi << 4) | lo);
    hex += 2;
  }
  if (hex[0] != '\0') return 0;              // odd number of hex digits
  return n;
}
