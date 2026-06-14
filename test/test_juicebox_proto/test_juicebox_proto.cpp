#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../src/lite/juicebox_proto.h"
#include <string.h>

static bool feed_str(JuiceBoxParser &p, const char *s, JuiceBoxFrame &out) {
  bool got = false;
  for (const char *c = s; *c; ++c) { if (p.feed((uint8_t)*c, out)) got = true; }
  return got;
}

TEST_CASE("parses a structured $ES frame terminated by CR") {
  JuiceBoxParser p; JuiceBoxFrame f;
  REQUIRE(feed_str(p, "$ES01C:S00,L00,T00,H00,A00,P000,F00\r", f));
  CHECK(strcmp(f.type, "ES") == 0);
  CHECK(strcmp(f.payload, "S00,L00,T00,H00,A00,P000,F00") == 0);
  CHECK(f.len == 0x1C);
}

TEST_CASE("parses a length-less $MD debug frame") {
  JuiceBoxParser p; JuiceBoxFrame f;
  REQUIRE(feed_str(p, "$MD:Back Online\n", f));
  CHECK(strcmp(f.type, "MD") == 0);
  CHECK(strcmp(f.payload, "Back Online") == 0);
}

TEST_CASE("the next $ flushes the previous frame (no terminator needed)") {
  JuiceBoxParser p; JuiceBoxFrame f;
  bool got = false;
  for (const char *c = "$PV002:20$ES"; *c; ++c) if (p.feed((uint8_t)*c, f)) got = true;
  REQUIRE(got);
  CHECK(strcmp(f.type, "PV") == 0);
  CHECK(strcmp(f.payload, "20") == 0);
}

TEST_CASE("garbage before '$' is discarded (resync)") {
  JuiceBoxParser p; JuiceBoxFrame f;
  REQUIRE(feed_str(p, "xyz\x01\x02$FW006:100102\r", f));
  CHECK(strcmp(f.type, "FW") == 0);
  CHECK(strcmp(f.payload, "100102") == 0);
}

TEST_CASE("a frame split across feeds still parses") {
  JuiceBoxParser p; JuiceBoxFrame f;
  CHECK_FALSE(feed_str(p, "$ES01C:S00,L00,", f));
  REQUIRE(feed_str(p, "T00,H00,A00,P000,F00\r", f));
  CHECK(strcmp(f.payload, "S00,L00,T00,H00,A00,P000,F00") == 0);
}

TEST_CASE("an over-long runaway line does not overflow") {
  JuiceBoxParser p; JuiceBoxFrame f;
  char big[256]; big[0] = '$'; big[1] = 'E'; big[2] = 'S';
  memset(big + 3, 'A', sizeof(big) - 4); big[sizeof(big) - 1] = '\0';
  for (char *c = big; *c; ++c) p.feed((uint8_t)*c, f);
  CHECK(true);
}

TEST_CASE("decodes all $ES fields including the 3-digit power field") {
  JuiceBoxStatus s;
  REQUIRE(juicebox_parse_es("S02,L01,T31,H00,A24,P240,F00", 28, s));
  CHECK(s.valid);
  CHECK(s.state == 2);
  CHECK(s.line  == 1);
  CHECK(s.temp  == 31);          // T is decimal
  CHECK(s.amps  == 24);          // A is decimal
  CHECK(s.power == 240);
  CHECK(s.offline_limit == 0);   // F = offline-limit echo, not fault
}

TEST_CASE("$ES S field is parsed as HEX, other fields stay decimal") {
  JuiceBoxStatus s;
  // Charging state 0x31 must decode to 49 (not decimal 31), while the decimal
  // fields beside it (A80, F80) stay 80 — proving per-field radix.
  REQUIRE(juicebox_parse_es("S31,L00,T00,H40,A80,P000,F80", 28, s));
  CHECK(s.state         == 0x31);  // == 49
  CHECK(s.amps          == 80);
  CHECK(s.offline_limit == 80);
  CHECK(juicebox_map_state(s.state) == LiteEvseState::Charging);

  // A hex-letter state (0x0A) round-trips too.
  REQUIRE(juicebox_parse_es("S0A,A00", 7, s));
  CHECK(s.state == 0x0A);          // == 10
}

TEST_CASE("$ES decode tolerates a missing trailing field") {
  JuiceBoxStatus s;
  REQUIRE(juicebox_parse_es("S00,A00", 7, s));
  CHECK(s.state == 0);
  CHECK(s.amps  == 0);
}

TEST_CASE("$ES decode rejects empty payload") {
  JuiceBoxStatus s;
  CHECK_FALSE(juicebox_parse_es("", 0, s));
}

TEST_CASE("maps hex JB state codes to canonical states (SERIAL_PROTOCOL.md §2a)") {
  // FIRM: charging + pre-charge => Charging; fault => Error.
  CHECK(juicebox_map_state(JB_S_CHARGING)  == LiteEvseState::Charging);   // 0x31
  CHECK(juicebox_map_state(JB_S_PRECHARGE) == LiteEvseState::Charging);   // 0x21
  CHECK(juicebox_map_state(JB_S_FAULT)     == LiteEvseState::Error);      // 0x05
  // Idle/poll codes => NotConnected (Connected needs the H field — follow-up).
  CHECK(juicebox_map_state(JB_S_INIT)    == LiteEvseState::NotConnected); // 0x00
  CHECK(juicebox_map_state(JB_S_READY)   == LiteEvseState::NotConnected); // 0x01
  CHECK(juicebox_map_state(JB_S_STANDBY) == LiteEvseState::NotConnected); // 0x02
  CHECK(juicebox_map_state(JB_S_IDLE)    == LiteEvseState::NotConnected); // 0x11
  // Anything outside the 7 valid codes => Unknown.
  CHECK(juicebox_map_state(0x04) == LiteEvseState::Unknown);
  CHECK(juicebox_map_state(0x99) == LiteEvseState::Unknown);
}

TEST_CASE("build_frame emits $<type><3hex len>:<payload>") {
  char buf[64];
  size_t n = juicebox_build_frame("PV", "20", buf, sizeof(buf));
  REQUIRE(n > 0);
  CHECK(strcmp(buf, "$PV002:20") == 0);
}

TEST_CASE("build_frame round-trips through the parser") {
  char buf[64];
  REQUIRE(juicebox_build_frame("ES", "S00,A00", buf, sizeof(buf)) > 0);
  char line[80]; snprintf(line, sizeof(line), "%s\r", buf);
  JuiceBoxParser p; JuiceBoxFrame f; bool got = false;
  for (char *c = line; *c; ++c) if (p.feed((uint8_t)*c, f)) got = true;
  REQUIRE(got);
  CHECK(strcmp(f.type, "ES") == 0);
  CHECK(strcmp(f.payload, "S00,A00") == 0);
}

TEST_CASE("build_frame refuses an undersized buffer") {
  char buf[4];
  CHECK(juicebox_build_frame("PV", "20", buf, sizeof(buf)) == 0);
}

TEST_CASE("amps-set builds a 2-digit payload and round-trips") {
  char buf[32];
  size_t n = juicebox_build_amps_set(24, buf, sizeof(buf));
  REQUIRE(n > 0);
  char line[40]; snprintf(line, sizeof(line), "%s\r", buf);
  JuiceBoxParser p; JuiceBoxFrame f; bool got = false;
  for (char *c = line; *c; ++c) if (p.feed((uint8_t)*c, f)) got = true;
  REQUIRE(got);
  CHECK(strcmp(f.payload, "24") == 0);
  CHECK(f.len == 2);
}

TEST_CASE("amps-set clamps an over-limit value to 79") {
  char buf[32];
  REQUIRE(juicebox_build_amps_set(100, buf, sizeof(buf)) > 0);
  char line[40]; snprintf(line, sizeof(line), "%s\r", buf);
  JuiceBoxParser p; JuiceBoxFrame f; bool got = false;
  for (char *c = line; *c; ++c) if (p.feed((uint8_t)*c, f)) got = true;
  REQUIRE(got);
  CHECK(strcmp(f.payload, "79") == 0);
}

TEST_CASE("canonical state names are stable") {
  CHECK(strcmp(lite_evse_state_name(LiteEvseState::NotConnected), "not_connected") == 0);
  CHECK(strcmp(lite_evse_state_name(LiteEvseState::Connected),    "connected")     == 0);
  CHECK(strcmp(lite_evse_state_name(LiteEvseState::Charging),     "charging")      == 0);
  CHECK(strcmp(lite_evse_state_name(LiteEvseState::Error),        "error")         == 0);
  CHECK(strcmp(lite_evse_state_name(LiteEvseState::Unknown),      "unknown")       == 0);
}
