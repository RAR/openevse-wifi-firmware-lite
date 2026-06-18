#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../src/lite/lite_rfid_proto.h"
#include <string.h>

TEST_CASE("SPI address byte: read sets bit0, write clears it") {
  CHECK(clrc663_addr_read(0x7F)  == 0xFF);  // (0x7F<<1)|1
  CHECK(clrc663_addr_write(0x7F) == 0xFE);  // (0x7F<<1)&0xFE
  CHECK(clrc663_addr_read(0x00)  == 0x01);
  CHECK(clrc663_addr_write(0x00) == 0x00);
  CHECK(clrc663_addr_read(CLRC663_REG_FIFODATA)  == 0x0B);  // (0x05<<1)|1
  CHECK(clrc663_addr_write(CLRC663_REG_FIFODATA) == 0x0A);
}

TEST_CASE("ISO14443A CRC_A matches the canonical HLTA vector") {
  // HLTA = {0x50, 0x00} + CRC_A; the standard wire bytes are 0x57, 0xCD.
  const uint8_t hlta[2] = {0x50, 0x00};
  CHECK(iso14443a_crc(hlta, 2) == 0xCD57);

  uint8_t out[4] = {0};
  size_t n = iso14443a_append_crc(hlta, 2, out, sizeof(out));
  REQUIRE(n == 4);
  CHECK(out[0] == 0x50);
  CHECK(out[1] == 0x00);
  CHECK(out[2] == 0x57);   // CRC_A LSB first
  CHECK(out[3] == 0xCD);
}

TEST_CASE("append_crc refuses an undersized buffer") {
  const uint8_t d[2] = {0x50, 0x00};
  uint8_t out[3] = {0};
  CHECK(iso14443a_append_crc(d, 2, out, sizeof(out)) == 0);  // needs 4
}

TEST_CASE("BCC is the XOR of the four cascade-level UID bytes") {
  const uint8_t uid[4] = {0x67, 0x8C, 0x42, 0x64};  // the stock badge UID
  CHECK(iso14443a_bcc(uid) == 0xCD);                 // 67^8C^42^64
}

TEST_CASE("UID -> hex is MSB-first upper-hex, no separators") {
  const uint8_t uid[4] = {0x67, 0x8C, 0x42, 0x64};
  char buf[16] = {0};
  size_t n = rfid_uid_to_hex(uid, 4, buf, sizeof(buf));
  CHECK(n == 8);
  CHECK(strcmp(buf, "678C4264") == 0);
}

TEST_CASE("UID -> hex handles 7-byte UIDs and rejects tiny buffers") {
  const uint8_t uid7[7] = {0x04, 0xAB, 0xCD, 0xEF, 0x12, 0x34, 0x56};
  char buf[15] = {0};
  CHECK(rfid_uid_to_hex(uid7, 7, buf, sizeof(buf)) == 14);
  CHECK(strcmp(buf, "04ABCDEF123456") == 0);

  char small[4];
  CHECK(rfid_uid_to_hex(uid7, 7, small, sizeof(small)) == 0);  // too small
  CHECK(small[0] == '\0');                                     // still NUL-terminated
}

TEST_CASE("hex -> UID round-trips and validates") {
  uint8_t out[10] = {0};
  size_t n = rfid_uid_from_hex("678C4264", out, sizeof(out));
  REQUIRE(n == 4);
  CHECK(out[0] == 0x67); CHECK(out[1] == 0x8C);
  CHECK(out[2] == 0x42); CHECK(out[3] == 0x64);

  // lower-case accepted
  CHECK(rfid_uid_from_hex("deadbeef", out, sizeof(out)) == 4);
  CHECK(out[0] == 0xDE);

  // malformed inputs -> 0
  CHECK(rfid_uid_from_hex("678", out, sizeof(out)) == 0);     // odd length
  CHECK(rfid_uid_from_hex("67ZZ", out, sizeof(out)) == 0);    // non-hex
  CHECK(rfid_uid_from_hex("00112233445566778899AABB", out, sizeof(out)) == 0);  // overflow (>10)
}
