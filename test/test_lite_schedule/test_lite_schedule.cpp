#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../src/lite/lite_schedule.h"
#include <string.h>

static LiteScheduleEvent ev(uint32_t id, uint8_t mask, uint8_t state, uint32_t sod) {
  LiteScheduleEvent e; memset(&e, 0, sizeof(e));
  e.id = id; e.day_mask = mask; e.state = state; e.sec_of_day = sod; return e;
}
#define DAY(i) (uint8_t)(1u << (i))   // 0=Sun..6=Sat

TEST_CASE("parse_time") {
  uint32_t s = 0;
  CHECK(lite_schedule_parse_time("23:00", s));    CHECK(s == 82800u);
  CHECK(lite_schedule_parse_time("06:30:15", s)); CHECK(s == 23415u);
  CHECK(lite_schedule_parse_time("00:00:00", s)); CHECK(s == 0u);
  CHECK_FALSE(lite_schedule_parse_time("24:00", s));
  CHECK_FALSE(lite_schedule_parse_time("12:60", s));
  CHECK_FALSE(lite_schedule_parse_time("9x", s));
  CHECK_FALSE(lite_schedule_parse_time("", s));
}

TEST_CASE("format_time") {
  char b[16];
  lite_schedule_format_time(82800, b, sizeof(b)); CHECK(strcmp(b, "23:00:00") == 0);
  lite_schedule_format_time(0, b, sizeof(b));     CHECK(strcmp(b, "00:00:00") == 0);
  lite_schedule_format_time(23415, b, sizeof(b)); CHECK(strcmp(b, "06:30:15") == 0);
}

TEST_CASE("day_index/day_name round trip") {
  CHECK(lite_schedule_day_index("sunday") == 0);
  CHECK(lite_schedule_day_index("saturday") == 6);
  CHECK(lite_schedule_day_index("monday") == 1);
  CHECK(lite_schedule_day_index("funday") == -1);
  for (int i = 0; i < 7; i++) CHECK(lite_schedule_day_index(lite_schedule_day_name(i)) == i);
  CHECK(lite_schedule_day_name(7) == nullptr);
}

TEST_CASE("upsert appends and replaces") {
  LiteSchedule s; memset(&s, 0, sizeof(s));
  CHECK(lite_schedule_upsert(s, ev(1, DAY(1), 1, 100))); CHECK(s.count == 1);
  CHECK(lite_schedule_upsert(s, ev(2, DAY(2), 2, 200))); CHECK(s.count == 2);
  // replace id 1 -> count unchanged, fields updated
  CHECK(lite_schedule_upsert(s, ev(1, DAY(3), 2, 300))); CHECK(s.count == 2);
  CHECK(lite_schedule_active_state(s, 3, 400) == 2);  // Wed 00:06:40, id1 fired at 300
}

TEST_CASE("upsert full table") {
  LiteSchedule s; memset(&s, 0, sizeof(s));
  for (uint32_t i = 1; i <= LITE_SCHEDULE_MAX_EVENTS; i++)
    CHECK(lite_schedule_upsert(s, ev(i, DAY(0), 1, i)));
  CHECK(s.count == LITE_SCHEDULE_MAX_EVENTS);
  CHECK_FALSE(lite_schedule_upsert(s, ev(999, DAY(0), 1, 1))); // new id, full
  CHECK(lite_schedule_upsert(s, ev(1, DAY(0), 2, 5)));         // replace existing, ok when full
}

TEST_CASE("remove compacts") {
  LiteSchedule s; memset(&s, 0, sizeof(s));
  lite_schedule_upsert(s, ev(1, DAY(1), 1, 100));
  lite_schedule_upsert(s, ev(2, DAY(2), 2, 200));
  lite_schedule_upsert(s, ev(3, DAY(3), 1, 300));
  CHECK(lite_schedule_remove(s, 2)); CHECK(s.count == 2);
  CHECK_FALSE(lite_schedule_remove(s, 2));   // already gone
  CHECK_FALSE(lite_schedule_remove(s, 99));  // never existed
}

TEST_CASE("active_state empty -> 0") {
  LiteSchedule s; memset(&s, 0, sizeof(s));
  CHECK(lite_schedule_active_state(s, 3, 50000) == 0);
}

TEST_CASE("active_state same-day two events") {
  LiteSchedule s; memset(&s, 0, sizeof(s));
  lite_schedule_upsert(s, ev(1, DAY(3), 2, 6 * 3600));   // Wed 06:00 Disabled
  lite_schedule_upsert(s, ev(2, DAY(3), 1, 23 * 3600));  // Wed 23:00 Active
  CHECK(lite_schedule_active_state(s, 3, 12 * 3600) == 2); // noon -> Disabled
  CHECK(lite_schedule_active_state(s, 3, 23 * 3600 + 1800) == 1); // 23:30 -> Active
}

TEST_CASE("active_state wraps across week") {
  LiteSchedule s; memset(&s, 0, sizeof(s));
  lite_schedule_upsert(s, ev(1, DAY(6), 1, 23 * 3600));  // Saturday 23:00 Active only
  CHECK(lite_schedule_active_state(s, 0, 5 * 3600) == 1); // Sunday 05:00 -> still Active (6h ago)
}

TEST_CASE("active_state multi-day mask") {
  LiteSchedule s; memset(&s, 0, sizeof(s));
  lite_schedule_upsert(s, ev(1, (uint8_t)(DAY(1) | DAY(3) | DAY(5)), 1, 8 * 3600)); // MWF 08:00
  CHECK(lite_schedule_active_state(s, 3, 9 * 3600) == 1);  // Wed 09:00 -> Active
}
