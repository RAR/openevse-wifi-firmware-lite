#ifdef OPENEVSE_LITE
// ---------------------------------------------------------------------------
// Lite link shims for the excluded full-firmware subsystems.
//
// The real EVSE core (evse_man.cpp / evse_monitor.cpp) references a handful of
// symbols that live in firmware modules we deliberately do NOT compile this
// slice (divert/solar, current-shaper, the main.cpp event bus). Rather than drag
// in those subsystems (and their MQTT/HTTP/config dependencies), we satisfy the
// exact named symbols with minimal stubs:
//
//   - event_send(JsonDocument&) / event_send(String&)  -> no-op event bus
//   - global `divert`  (DivertTask)         -> inert; isActive() == false
//   - global `shaper`  (CurrentShaperTask)  -> inert; getState()  == false
//
// The two globals are real DivertTask/CurrentShaperTask instances (so the EVSE
// core's `divert.isActive()` / `shaper.getState()` calls bind), but every
// behavioural method is a stub — they never run as MicroTasks (we don't
// startTask() them) and their setup()/loop() are empty. Slice-2 replaces these
// with the genuine subsystems if/when wanted.
// ---------------------------------------------------------------------------
#include <Arduino.h>
#include <ArduinoJson.h>

#include "evse_man.h"
#include "event.h"
#include "divert.h"
#include "current_shaper.h"

// The EVSE core wires divert/shaper against the global EvseManager (defined in
// main_lite.cpp). Mirror main.cpp's construction.
extern EvseManager evse;

// -------- event bus (main.cpp in the full firmware) --------
void event_send(String &) { /* no-op: no MQTT/web event sink this slice */ }
void event_send(JsonDocument &) { /* no-op */ }

// -------- DivertTask stub (divert.cpp in the full firmware) --------
DivertTask::DivertTask(EvseManager &e) :
  _evse(&e),
  _mode(DivertMode::Normal),
  _state(EvseState::None),
  _last_update(0),
  _charge_rate(0),
  _evseState(this),
  _available_current(0),
  _smoothed_available_current(0),
  _min_charge_end(0)
{
}
DivertTask::~DivertTask() {}
void DivertTask::setup() {}
unsigned long DivertTask::loop(MicroTasks::WakeReason) { return MicroTask.Infinate; }
bool DivertTask::isActive() { return false; }

// -------- CurrentShaperTask stub (current_shaper.cpp in the full firmware) --------
CurrentShaperTask::CurrentShaperTask() :
  MicroTasks::Task(),
  _evse(nullptr),
  _enabled(false),
  _changed(false),
  _max_pwr(0),
  _live_pwr(0),
  _smoothed_live_pwr(0),
  _chg_cur(0),
  _max_cur(0),
  _timer(0),
  _pause_timer(0),
  _updated(false)
{
}
CurrentShaperTask::~CurrentShaperTask() {}
void CurrentShaperTask::setup() {}
unsigned long CurrentShaperTask::loop(MicroTasks::WakeReason) { return MicroTask.Infinate; }
bool CurrentShaperTask::getState() { return false; }

// -------- the globals the EVSE core names --------
DivertTask divert(evse);
CurrentShaperTask shaper;

#endif // OPENEVSE_LITE
