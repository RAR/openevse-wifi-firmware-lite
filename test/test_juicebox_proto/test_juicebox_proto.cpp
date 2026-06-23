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

TEST_CASE("juicebox_wr_code parses the leading $WR fault number") {
  CHECK(juicebox_wr_code("005:Pilot Signal Gen Fail:") == 5);
  CHECK(juicebox_wr_code("003:No GND:") == 3);
  CHECK(juicebox_wr_code("101:Ground Fault Int:") == 101);
  CHECK(juicebox_wr_code("No GND") == -1);     // no leading digits
  CHECK(juicebox_wr_code("") == -1);
  CHECK(juicebox_wr_code(nullptr) == -1);
}

TEST_CASE("juicebox_fault_openevse_state maps faults to the closest OpenEVSE code") {
  CHECK(juicebox_fault_openevse_state(3)   == 7);  // No GND -> no ground
  CHECK(juicebox_fault_openevse_state(6)   == 9);  // GFI Auto Test Fail -> GFCI self-test
  CHECK(juicebox_fault_openevse_state(7)   == 8);  // Relay Stuck Closed -> stuck relay
  CHECK(juicebox_fault_openevse_state(8)   == 6);  // Ground Fault Int Lockout -> GFCI fault
  CHECK(juicebox_fault_openevse_state(101) == 6);  // Ground Fault Int -> GFCI fault
  CHECK(juicebox_fault_openevse_state(102) == 8);  // Relay Stuck Open -> stuck relay
  CHECK(juicebox_fault_openevse_state(5)   == 5);  // Pilot Signal Gen Fail -> diode (nearest, NOT relay)
  CHECK(juicebox_fault_openevse_state(4)   == 5);  // Short Circuit Pilot -> diode (nearest)
  CHECK(juicebox_fault_openevse_state(105) == 4);  // Vehicle vent req -> vent required (HW-confirmed)
  CHECK(juicebox_fault_openevse_state(1)   == 9);  // FW Self Tests Failed -> self-test
  CHECK(juicebox_fault_openevse_state(999) == 8);  // unknown -> generic fault slot
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

TEST_CASE("maps J1772 pilot-state S codes to canonical states (HW-confirmed 2026-06-22)") {
  // S field = J1772 pilot state, stepped on a tester: A=0x00 B=0x11 C=0x02 D=0x05.
  CHECK(juicebox_map_state(JB_S_A) == LiteEvseState::NotConnected); // 0x00 J1772 A: no vehicle
  CHECK(juicebox_map_state(JB_S_B) == LiteEvseState::Connected);    // 0x11 J1772 B: plugged, idle
  CHECK(juicebox_map_state(JB_S_C) == LiteEvseState::Charging);     // 0x02 J1772 C: charging
  CHECK(juicebox_map_state(JB_S_D) == LiteEvseState::Error);        // 0x05 J1772 D: vent (->$WR state 4)
  CHECK(juicebox_map_state(JB_S_READY) == LiteEvseState::NotConnected); // 0x01 transitional, no vehicle
  // Legacy static-RE charging codes kept as defensive Charging (unseen on HW).
  CHECK(juicebox_map_state(JB_S_CHARGING)  == LiteEvseState::Charging);   // 0x31
  CHECK(juicebox_map_state(JB_S_PRECHARGE) == LiteEvseState::Charging);   // 0x21
  // Anything else => Unknown.
  CHECK(juicebox_map_state(0x04) == LiteEvseState::Unknown);
  CHECK(juicebox_map_state(0x99) == LiteEvseState::Unknown);
}

TEST_CASE("frame checksum reproduces captured stock trailers (both directions)") {
  // (frame-without-trailer, expected 3-char trailer) — verbatim from the 2026-06-17
  // dual-UART capture (20260617-134204-dualtap-merged.log). The CRC covers the $/~ prefix.
  struct { const char *frame; const char *trailer; } v[] = {
    {"~AL002:40", ">Oz"}, {"~OL002:40", "AzL"}, {"~LK002:00", "CiT"},
    {"~PV002:20", "?CV"}, {"~ES000:", "@{S"}, {"~LK002:01", nullptr},
    {"$ES01C:S05,L01,T00,H40,A40,P100,F40", "GNf"},
    {"$WR00B:003:No GND", ">jJ"}, {"$FW006:100102", "HJP"},
  };
  for (auto &e : v) {
    if (!e.trailer) continue;   // ~LK002:01 has no captured trailer; covered by build test
    char tr[4];
    juicebox_encode_trailer(juicebox_crc(e.frame, strlen(e.frame)), tr);
    CHECK(strcmp(tr, e.trailer) == 0);
  }
}

TEST_CASE("build_frame emits ~<type><3hex len>:<payload>:<crc>: byte-exact vs stock") {
  char buf[64];
  REQUIRE(juicebox_build_frame("PV", "20", buf, sizeof(buf)) > 0);
  CHECK(strcmp(buf, "~PV002:20:?CV:") == 0);
}

TEST_CASE("build_frame refuses an undersized buffer") {
  char buf[8];   // need 15 for ~PV002:20:?CV:\0
  CHECK(juicebox_build_frame("PV", "20", buf, sizeof(buf)) == 0);
}

TEST_CASE("amps-set is byte-exact vs the captured stock ~AL002:40 frame") {
  char buf[32];
  REQUIRE(juicebox_build_amps_set(40, buf, sizeof(buf)) > 0);
  CHECK(strcmp(buf, "~AL002:40:>Oz:") == 0);
}

TEST_CASE("amps-set clamps an over-limit value to 79 (with a valid trailer)") {
  char buf[32];
  REQUIRE(juicebox_build_amps_set(100, buf, sizeof(buf)) > 0);
  char tr[4]; juicebox_encode_trailer(juicebox_crc("~AL002:79", 9), tr);
  char want[32]; snprintf(want, sizeof(want), "~AL002:79:%s:", tr);
  CHECK(strcmp(buf, want) == 0);
}

TEST_CASE("lock + offline-limit commands are byte-exact vs stock") {
  char buf[32];
  REQUIRE(juicebox_build_lock(false, buf, sizeof(buf)) > 0);
  CHECK(strcmp(buf, "~LK002:00:CiT:") == 0);   // unlock/enable
  REQUIRE(juicebox_build_lock(true, buf, sizeof(buf)) > 0);
  CHECK(strncmp(buf, "~LK002:01:", 10) == 0);  // lock/stop (trailer not in capture)
  CHECK(strlen(buf) == 14);                    // ~LK002:01:XXX:
  REQUIRE(juicebox_build_offline_limit(40, buf, sizeof(buf)) > 0);
  CHECK(strcmp(buf, "~OL002:40:AzL:") == 0);
}

TEST_CASE("canonical state names are stable") {
  CHECK(strcmp(lite_evse_state_name(LiteEvseState::NotConnected), "not_connected") == 0);
  CHECK(strcmp(lite_evse_state_name(LiteEvseState::Connected),    "connected")     == 0);
  CHECK(strcmp(lite_evse_state_name(LiteEvseState::Charging),     "charging")      == 0);
  CHECK(strcmp(lite_evse_state_name(LiteEvseState::Error),        "error")         == 0);
  CHECK(strcmp(lite_evse_state_name(LiteEvseState::Unknown),      "unknown")       == 0);
}

TEST_CASE("tilde (~) is a second frame-start; start delimiter is recorded") {
  JuiceBoxParser p; JuiceBoxFrame f;
  // ~JV:?$  — the MCU's version query; the trailing '$' closes the ~ frame.
  REQUIRE(feed_str(p, "~JV:?$\r\n", f));
  CHECK(f.start == '~');
  CHECK(strcmp(f.type, "JV") == 0);
  CHECK(strcmp(f.payload, "?") == 0);
}
TEST_CASE("$ frames still report start '$'") {
  JuiceBoxParser p; JuiceBoxFrame f;
  REQUIRE(feed_str(p, "$ES01C:S00,L00,T00,H00,A00,P000,F00\r", f));
  CHECK(f.start == '$');
  CHECK(strcmp(f.type, "ES") == 0);
}
TEST_CASE("a ~ frame interrupts and flushes a pending ~ frame") {
  JuiceBoxParser p; JuiceBoxFrame f;
  // No CR/LF: the next '~' should flush the first (mirrors the '$' resync behavior).
  bool got = false;
  for (const char *c = "~JV:?~JV:!1"; *c; ++c) if (p.feed((uint8_t)*c, f)) { got = true; break; }
  REQUIRE(got);
  CHECK(f.start == '~');
  CHECK(strcmp(f.type, "JV") == 0);
}
