#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../src/lite/lite_provision.h"
#include <cstring>

TEST_CASE("decide: connected always wins") {
  CHECK(lite_provision_decide(true,  true, 0, 999999, 60000) == LiteProvisionAction::StaOnline);
  CHECK(lite_provision_decide(false, true, 0, 10,     60000) == LiteProvisionAction::StaOnline);
}
TEST_CASE("decide: no creds -> AP immediately") {
  CHECK(lite_provision_decide(false, false, 0, 0, 60000) == LiteProvisionAction::EnterAp);
}
TEST_CASE("decide: creds + connecting within timeout -> wait") {
  CHECK(lite_provision_decide(true, false, 1000, 5000, 60000) == LiteProvisionAction::StaWait); // elapsed 4000
}
TEST_CASE("decide: creds + timeout reached -> AP (inclusive)") {
  CHECK(lite_provision_decide(true, false, 0, 60000, 60000) == LiteProvisionAction::EnterAp);
  CHECK(lite_provision_decide(true, false, 0, 59999, 60000) == LiteProvisionAction::StaWait);
}
TEST_CASE("decide: elapsed is wrap-safe") {
  uint32_t start = 0xFFFFF000u, now = 0x00000400u; // delta = 0x1400 = 5120
  CHECK(lite_provision_decide(true, false, start, now, 60000) == LiteProvisionAction::StaWait);
  CHECK(lite_provision_decide(true, false, start, now, 5000)  == LiteProvisionAction::EnterAp);
}
TEST_CASE("retry: only when creds failed, on interval, wrap-safe") {
  CHECK_FALSE(lite_provision_should_retry_sta(false, 0, 999999, 1000)); // no creds -> never
  CHECK(lite_provision_should_retry_sta(true, 0, 1000, 1000));          // exactly interval -> yes
  CHECK_FALSE(lite_provision_should_retry_sta(true, 0, 999, 1000));     // before -> no
  uint32_t s = 0xFFFFFF00u, n = 0x00000064u;                            // delta 356
  CHECK(lite_provision_should_retry_sta(true, s, n, 300));              // 356>=300
  CHECK_FALSE(lite_provision_should_retry_sta(true, s, n, 400));        // 356<400
}
TEST_CASE("ap ssid format + truncation") {
  char b[32]; lite_provision_ap_ssid("a1b2c3", b, sizeof(b));
  CHECK(std::strcmp(b, "OpenEVSE-Lite-a1b2c3") == 0);
  char s[10]; lite_provision_ap_ssid("a1b2c3", s, sizeof(s));           // cap 10 -> 9 chars + NUL
  CHECK(std::strlen(s) == 9); CHECK(s[9] == '\0');
}
TEST_CASE("enc map: 0 open, else secured") {
  CHECK(lite_provision_enc(0) == 0);
  CHECK(lite_provision_enc(3) == 1);
  CHECK(lite_provision_enc(7) == 1);
}
TEST_CASE("url decode: percent, plus, passthrough") {
  char o[64];
  CHECK(lite_url_decode("a%20b+c", 7, o, sizeof(o)) == 5); CHECK(std::strcmp(o, "a b c") == 0);
  CHECK(lite_url_decode("%41%42", 6, o, sizeof(o)) == 2);  CHECK(std::strcmp(o, "AB") == 0);
  CHECK(lite_url_decode("ab%", 3, o, sizeof(o)) == 3);     CHECK(std::strcmp(o, "ab%") == 0); // malformed tail -> literal
  CHECK(lite_url_decode("%2", 2, o, sizeof(o)) == 2);      CHECK(std::strcmp(o, "%2") == 0);
}
