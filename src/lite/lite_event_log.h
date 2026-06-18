#pragma once
#include <stdint.h>
#include <stddef.h>

// Event log = a fixed RAM ring of EVSE state-change snapshots, serving the OpenEVSE
// /logs API. lite has no LittleFS (the standard EventLog uses rotating files), so the
// store is RAM; completed-session history is persisted to the kvs KVDB blob by the
// caller on the session-complete edge. NOT guarded by OPENEVSE_LITE — native-tested.

#ifndef LITE_EVENTLOG_CAPACITY
#define LITE_EVENTLOG_CAPACITY 40   // sizeof(entry)*capacity must stay < ~3.5 KB (one kvs sector)
#endif

enum class LiteEventType : uint8_t { Information = 0, Notification = 1, Warning = 2 };

#define LITE_EVENTLOG_RFID_MAX 15   // UID hex chars + NUL

struct LiteEventLogEntry {
  uint32_t epoch;        // UTC seconds at event time
  uint32_t evseFlags;
  uint32_t energyWh;     // session Wh
  uint32_t elapsedS;     // session seconds
  uint8_t  type;         // LiteEventType
  uint8_t  managerState; // EvseState int (0 none / 1 active / 2 disabled)
  uint8_t  evseState;    // OpenEVSE numeric state code
  uint8_t  divertMode;   // 1 normal / 2 eco
  uint8_t  shaper;       // 0/1
  int8_t   soc;          // -1 = unavailable
  int16_t  tempC;        // whole degC (INT16_MIN = invalid / no sensor)
  uint16_t pilotA;       // A
  char     rfid[LITE_EVENTLOG_RFID_MAX]; // UID hex, "" = none
};

class LiteEventLog {
public:
  void   clear();
  void   push(const LiteEventLogEntry &e);     // append; overwrites the oldest when full
  size_t count() const { return _count; }      // number stored (<= capacity)
  const LiteEventLogEntry &at(size_t i) const;  // oldest..newest, i in [0, count())

  // Persistence blob: [u8 version=1][u8 count][count * LiteEventLogEntry] (oldest..newest).
  size_t toBlob(void *buf, size_t cap) const;   // bytes written, or 0 if buf too small
  bool   fromBlob(const void *buf, size_t len); // validate version+count; false (and clear) on mismatch

private:
  LiteEventLogEntry _buf[LITE_EVENTLOG_CAPACITY];
  size_t _head = 0;    // index of the oldest entry
  size_t _count = 0;
};

static const uint8_t LITE_EVENTLOG_BLOB_VERSION = 1;
