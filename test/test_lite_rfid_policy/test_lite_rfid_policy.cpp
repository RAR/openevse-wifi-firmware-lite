#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../src/lite/lite_rfid_policy.h"
#include <string.h>

TEST_CASE("allowlist parse: comma / space / newline separated, normalized upper") {
  LiteRfidAllowlist a;
  uint8_t n = lite_rfid_allowlist_parse("678c4264, deadbeef\n04ABCDEF12", &a);
  CHECK(n == 3);
  CHECK(strcmp(a.list[0], "678C4264") == 0);   // upper-cased
  CHECK(strcmp(a.list[1], "DEADBEEF") == 0);
  CHECK(strcmp(a.list[2], "04ABCDEF12") == 0);
}

TEST_CASE("allowlist parse: skips malformed tokens") {
  LiteRfidAllowlist a;
  // "678" odd-length, "ZZZZ" non-hex, "" empty -> all skipped; two valid remain
  uint8_t n = lite_rfid_allowlist_parse("678;ZZZZ;;1234;ABCD", &a);
  CHECK(n == 2);
  CHECK(strcmp(a.list[0], "1234") == 0);
  CHECK(strcmp(a.list[1], "ABCD") == 0);
}

TEST_CASE("allowlist parse: caps at LITE_RFID_MAX_ALLOW") {
  // build a long list of 20 valid 2-char UIDs
  char buf[128] = {0};
  for (int i = 0; i < 20; ++i) { char t[4]; snprintf(t, sizeof(t), "A%d,", i % 10); strcat(buf, "AB,"); }
  LiteRfidAllowlist a;
  uint8_t n = lite_rfid_allowlist_parse(buf, &a);
  CHECK(n == LITE_RFID_MAX_ALLOW);
}

TEST_CASE("allowlist membership is case-insensitive") {
  LiteRfidAllowlist a;
  lite_rfid_allowlist_parse("678C4264,DEADBEEF", &a);
  CHECK(lite_rfid_uid_allowed("678C4264", &a));
  CHECK(lite_rfid_uid_allowed("678c4264", &a));   // lower-case candidate
  CHECK(lite_rfid_uid_allowed("deadbeef", &a));
  CHECK_FALSE(lite_rfid_uid_allowed("12345678", &a));
  CHECK_FALSE(lite_rfid_uid_allowed("", &a));
}

TEST_CASE("empty allowlist denies everything") {
  LiteRfidAllowlist a; a.count = 0;
  CHECK_FALSE(lite_rfid_uid_allowed("678C4264", &a));
}

TEST_CASE("decide: feature off -> never acts") {
  LiteRfidDecideIn in{false, true, true, false};
  CHECK(lite_rfid_decide(in) == LiteRfidAction::None);
}

TEST_CASE("decide: listed tag authorizes") {
  LiteRfidDecideIn in{true, true, true, false};
  CHECK(lite_rfid_decide(in) == LiteRfidAction::Authorize);
}

TEST_CASE("decide: unlisted tag is denied") {
  LiteRfidDecideIn in{true, true, false, false};
  CHECK(lite_rfid_decide(in) == LiteRfidAction::Deny);
}

TEST_CASE("decide: no card -> no change") {
  LiteRfidDecideIn in{true, false, false, false};
  CHECK(lite_rfid_decide(in) == LiteRfidAction::None);
}

TEST_CASE("decide: duplicate swipe while authorized is ignored") {
  LiteRfidDecideIn in{true, true, true, true};   // present + allowed but already authorized
  CHECK(lite_rfid_decide(in) == LiteRfidAction::None);
}

TEST_CASE("status reset zeroes the struct") {
  LiteRfidStatus s;
  memset(&s, 0xFF, sizeof(s));
  lite_rfid_status_reset(&s);
  CHECK(s.enabled == false);
  CHECK(s.reader_ok == false);
  CHECK(s.failure_count == 0);
  CHECK(s.authorized == false);
  CHECK(s.last_uid[0] == '\0');
}
