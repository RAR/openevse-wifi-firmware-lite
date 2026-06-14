#pragma once
#include <stdint.h>
#include <stddef.h>

#define LITE_SCHEDULE_MAX_EVENTS 16

// One weekly schedule event. state uses EvseState values (1=Active, 2=Disabled); id 0 is
// reserved "empty". Fixed 12-byte layout for the persisted blob.
struct LiteScheduleEvent {
  uint32_t id;          // client-assigned, nonzero
  uint8_t  day_mask;    // bit0=Sun .. bit6=Sat
  uint8_t  state;       // 1=Active, 2=Disabled
  uint8_t  pad[2];      // zero; keeps the struct 4-byte aligned and blob-stable
  uint32_t sec_of_day;  // 0..86399 fire time (local)
};

struct LiteSchedule {
  LiteScheduleEvent events[LITE_SCHEDULE_MAX_EVENTS];
  uint32_t count;       // populated slots [0..MAX]
};

// Most-recently-fired event's state as of (dayOfWeek 0=Sun..6=Sat, secOfDay 0..86399).
// Returns 0 (EvseState::None) when no event applies. Pure.
uint8_t lite_schedule_active_state(const LiteSchedule &s, int dayOfWeek, uint32_t secOfDay);

// Upsert by id (replace same-id, else append). False if full and id not present. Pure.
bool lite_schedule_upsert(LiteSchedule &s, const LiteScheduleEvent &e);
// Remove by id. False if absent. Pure. Compacts the array (order not preserved).
bool lite_schedule_remove(LiteSchedule &s, uint32_t id);

// "HH:MM" or "HH:MM:SS" -> seconds of day. False on malformed/out-of-range. Pure.
bool lite_schedule_parse_time(const char *hhmmss, uint32_t &secOfDay);
// secOfDay -> "HH:MM:SS" (cap >= 9). Pure.
void lite_schedule_format_time(uint32_t secOfDay, char *buf, size_t cap);
// lowercase day name (e.g. "monday") -> bit index 0..6 (Sun=0); -1 unknown. Pure.
int  lite_schedule_day_index(const char *name);
// bit index 0..6 -> lowercase day name (static string); nullptr if out of range. Pure.
const char *lite_schedule_day_name(int index);
