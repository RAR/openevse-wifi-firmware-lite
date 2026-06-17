#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../src/lite/lite_console_proto.h"
#include <string.h>

TEST_CASE("Sec-WebSocket-Accept matches the RFC6455 §1.3 example vector") {
  // The canonical RFC6455 example: key "dGhlIHNhbXBsZSBub25jZQ==" yields the
  // accept "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=".
  char out[32] = {0};
  size_t n = lite_ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==", out, sizeof(out));
  CHECK(n == 28);
  CHECK(strcmp(out, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0);
}

TEST_CASE("accept-key rejects a too-small buffer / null key") {
  char small[8];
  CHECK(lite_ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==", small, sizeof(small)) == 0);
  char out[32];
  CHECK(lite_ws_accept_key(nullptr, out, sizeof(out)) == 0);
}

TEST_CASE("header length picks the right RFC6455 length encoding") {
  CHECK(lite_ws_header_len(0)      == 2);
  CHECK(lite_ws_header_len(125)    == 2);
  CHECK(lite_ws_header_len(126)    == 4);
  CHECK(lite_ws_header_len(0xFFFF) == 4);
  CHECK(lite_ws_header_len(0x10000) == 10);
}

TEST_CASE("encodes a short TEXT frame (7-bit length, unmasked)") {
  const char *msg = "hi";
  uint8_t out[16] = {0};
  size_t n = lite_ws_encode_text((const uint8_t *)msg, 2, out, sizeof(out));
  REQUIRE(n == 4);            // 2-byte header + 2-byte payload
  CHECK(out[0] == 0x81);      // FIN=1, opcode=0x1 (text)
  CHECK(out[1] == 0x02);      // unmasked (bit7=0), len=2
  CHECK(out[2] == 'h');
  CHECK(out[3] == 'i');
}

TEST_CASE("encodes a 16-bit-length TEXT frame at the 126 boundary") {
  uint8_t payload[200];
  memset(payload, 'A', sizeof(payload));
  uint8_t out[256] = {0};
  size_t n = lite_ws_encode_text(payload, 200, out, sizeof(out));
  REQUIRE(n == 4 + 200);
  CHECK(out[0] == 0x81);
  CHECK(out[1] == 126);       // extended 16-bit length follows
  CHECK(out[2] == 0x00);      // 200 = 0x00C8, big-endian
  CHECK(out[3] == 0xC8);
  CHECK(out[4] == 'A');
}

TEST_CASE("encode fails when the output buffer cannot hold header + payload") {
  const char *msg = "hello";
  uint8_t tiny[4];            // need 2 + 5 = 7
  CHECK(lite_ws_encode_text((const uint8_t *)msg, 5, tiny, sizeof(tiny)) == 0);
}

TEST_CASE("encode_header writes just the header bytes for both length forms") {
  uint8_t h[10] = {0};
  CHECK(lite_ws_encode_header(2, h) == 2);
  CHECK(h[0] == 0x81);
  CHECK(h[1] == 0x02);
  uint8_t h2[10] = {0};
  CHECK(lite_ws_encode_header(200, h2) == 4);
  CHECK(h2[0] == 0x81);
  CHECK(h2[1] == 126);
  CHECK(h2[2] == 0x00);
  CHECK(h2[3] == 0xC8);
}

// Build a client->server masked frame: [0x80|opcode][0x80|len][mask4][masked payload].
static size_t build_masked(uint8_t opcode, const char *payload, size_t plen,
                           const uint8_t mask[4], uint8_t *out) {
  size_t o = 0;
  out[o++] = 0x80 | opcode;       // FIN + opcode
  out[o++] = 0x80 | (uint8_t)plen; // mask bit + 7-bit len (test payloads are < 126)
  for (int i = 0; i < 4; i++) out[o++] = mask[i];
  for (size_t i = 0; i < plen; i++) out[o++] = (uint8_t)payload[i] ^ mask[i % 4];
  return o;
}

TEST_CASE("decodes a masked client TEXT frame and unmasks the payload") {
  const char *msg = "{\"ping\":1}";   // exactly what the nightshift UI sends
  size_t plen = 10;
  const uint8_t mask[4] = { 0x12, 0x34, 0x56, 0x78 };
  uint8_t frame[32];
  size_t flen = build_masked(0x1, msg, plen, mask, frame);

  LiteWsFrame fr;
  size_t consumed = lite_ws_decode_frame(frame, flen, &fr);
  CHECK(consumed == flen);
  CHECK(fr.opcode == 0x1);
  CHECK(fr.fin == true);
  CHECK(fr.payload_len == plen);
  CHECK(memcmp(fr.payload, msg, plen) == 0);   // unmasked in place
}

TEST_CASE("decode returns 0 on an incomplete frame (needs more bytes)") {
  const uint8_t mask[4] = { 0x01, 0x02, 0x03, 0x04 };
  uint8_t frame[32];
  size_t flen = build_masked(0x1, "hello", 5, mask, frame);
  LiteWsFrame fr;
  CHECK(lite_ws_decode_frame(frame, flen - 1, &fr) == 0);  // one byte short
}

TEST_CASE("decodes a close frame opcode") {
  const uint8_t mask[4] = { 0xAA, 0xBB, 0xCC, 0xDD };
  uint8_t frame[16];
  size_t flen = build_masked(0x8, "", 0, mask, frame);   // empty-payload close
  LiteWsFrame fr;
  CHECK(lite_ws_decode_frame(frame, flen, &fr) == flen);
  CHECK(fr.opcode == 0x8);
}
