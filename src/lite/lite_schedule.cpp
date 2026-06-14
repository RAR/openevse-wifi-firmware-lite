#include "lite_schedule.h"
#include <stdio.h>
#include <string.h>

static_assert(sizeof(LiteScheduleEvent) == 12, "LiteScheduleEvent must be 12 bytes (blob-stable)");
static_assert(sizeof(LiteSchedule) == 12 * LITE_SCHEDULE_MAX_EVENTS + 4,
              "LiteSchedule blob size changed");

uint8_t lite_schedule_active_state(const LiteSchedule &s, int dayOfWeek, uint32_t secOfDay) {
  const uint32_t WEEK = 7u * 86400u;
  uint32_t now = (uint32_t)dayOfWeek * 86400u + secOfDay;
  uint32_t bestAgo = WEEK; uint8_t bestState = 0;
  for (uint32_t i = 0; i < s.count && i < LITE_SCHEDULE_MAX_EVENTS; i++) {
    const LiteScheduleEvent &e = s.events[i];
    if (e.id == 0) continue;
    for (int d = 0; d < 7; d++) {
      if (!(e.day_mask & (1u << d))) continue;
      uint32_t fire = (uint32_t)d * 86400u + e.sec_of_day;
      uint32_t ago  = (now + WEEK - fire) % WEEK;
      if (ago < bestAgo) { bestAgo = ago; bestState = e.state; }
    }
  }
  return bestState;
}

bool lite_schedule_upsert(LiteSchedule &s, const LiteScheduleEvent &e) {
  for (uint32_t i = 0; i < s.count && i < LITE_SCHEDULE_MAX_EVENTS; i++) {
    if (s.events[i].id == e.id) { s.events[i] = e; return true; }
  }
  if (s.count >= LITE_SCHEDULE_MAX_EVENTS) return false;
  s.events[s.count++] = e;
  return true;
}

bool lite_schedule_remove(LiteSchedule &s, uint32_t id) {
  for (uint32_t i = 0; i < s.count && i < LITE_SCHEDULE_MAX_EVENTS; i++) {
    if (s.events[i].id == id) {
      s.events[i] = s.events[s.count - 1];   // compact: move last into the gap
      memset(&s.events[s.count - 1], 0, sizeof(LiteScheduleEvent));
      s.count--;
      return true;
    }
  }
  return false;
}

bool lite_schedule_parse_time(const char *t, uint32_t &secOfDay) {
  if (!t) return false;
  int h = -1, m = -1, sec = 0;
  int n = sscanf(t, "%d:%d:%d", &h, &m, &sec);
  if (n < 2) return false;
  if (h < 0 || h > 23 || m < 0 || m > 59 || sec < 0 || sec > 59) return false;
  secOfDay = (uint32_t)h * 3600u + (uint32_t)m * 60u + (uint32_t)sec;
  return true;
}

void lite_schedule_format_time(uint32_t secOfDay, char *buf, size_t cap) {
  if (!buf || cap == 0) return;
  secOfDay %= 86400u;
  unsigned h = secOfDay / 3600u, m = (secOfDay % 3600u) / 60u, s = secOfDay % 60u;
  snprintf(buf, cap, "%02u:%02u:%02u", h, m, s);
}

static const char *const kDayNames[7] = {
  "sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday"
};

int lite_schedule_day_index(const char *name) {
  if (!name) return -1;
  for (int i = 0; i < 7; i++) if (strcmp(name, kDayNames[i]) == 0) return i;
  return -1;
}

const char *lite_schedule_day_name(int index) {
  return (index >= 0 && index < 7) ? kDayNames[index] : nullptr;
}
