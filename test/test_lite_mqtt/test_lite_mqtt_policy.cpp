#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../src/lite/lite_mqtt_policy.h"

TEST_CASE("should_publish: never-published is always due") {
  CHECK(mqtt_should_publish(0, 0,     30000, 5000, false) == true);
  CHECK(mqtt_should_publish(0, 12345, 30000, 5000, false) == true);
}
TEST_CASE("should_publish: idle period gating") {
  CHECK(mqtt_should_publish(1000, 1000 + 29999, 30000, 5000, false) == false);
  CHECK(mqtt_should_publish(1000, 1000 + 30000, 30000, 5000, false) == true);
}
TEST_CASE("should_publish: charging uses the shorter period") {
  CHECK(mqtt_should_publish(1000, 1000 + 4999, 30000, 5000, true) == false);
  CHECK(mqtt_should_publish(1000, 1000 + 5000, 30000, 5000, true) == true);
  // same elapsed, idle would NOT be due:
  CHECK(mqtt_should_publish(1000, 1000 + 5000, 30000, 5000, false) == false);
}
TEST_CASE("should_publish: unsigned wrap is due") {
  uint32_t last = 0xFFFFF000u, now = 0x00000400u; // delta 0x1400 = 5120
  CHECK(mqtt_should_publish(last, now, 30000, 5000, true)  == true);  // >= 5000
  CHECK(mqtt_should_publish(last, now, 30000, 5000, false) == false); // < 30000
}
TEST_CASE("backoff_due") {
  CHECK(mqtt_backoff_due(0, 0, 5000)          == true);   // never attempted
  CHECK(mqtt_backoff_due(1000, 1000+4999, 5000) == false);
  CHECK(mqtt_backoff_due(1000, 1000+5000, 5000) == true);
}
TEST_CASE("topic_join: single slash, trims extras") {
  CHECK(mqtt_topic_join("openevse", "amp")    == "openevse/amp");
  CHECK(mqtt_topic_join("openevse/", "amp")   == "openevse/amp");
  CHECK(mqtt_topic_join("openevse", "/amp")   == "openevse/amp");
  CHECK(mqtt_topic_join("openevse/", "/amp")  == "openevse/amp");
  CHECK(mqtt_topic_join("", "amp")            == "amp");
  CHECK(mqtt_topic_join("openevse", "")       == "openevse");
}
TEST_CASE("default_base: lowercases and prefixes") {
  CHECK(mqtt_default_base("A1B2C3") == "openevse-a1b2c3");
}
