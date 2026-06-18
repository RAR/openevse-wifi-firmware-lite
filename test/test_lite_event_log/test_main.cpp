#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../src/lite/lite_event_log.h"
#include <string.h>

static LiteEventLogEntry mk(uint32_t epoch, uint8_t evseState) {
  LiteEventLogEntry e;
  memset(&e, 0, sizeof(e));
  e.epoch = epoch;
  e.evseState = evseState;
  e.soc = -1;
  e.tempC = 215;            // 21.5 -> stored whole, here just a marker
  e.energyWh = epoch * 2;   // deterministic payload for ordering checks
  return e;
}

TEST_CASE("empty log") {
  LiteEventLog log;
  CHECK(log.count() == 0);
}

TEST_CASE("push + ordering oldest..newest") {
  LiteEventLog log;
  log.push(mk(100, 2));
  log.push(mk(200, 3));
  log.push(mk(300, 1));
  CHECK(log.count() == 3);
  CHECK(log.at(0).epoch == 100);
  CHECK(log.at(1).epoch == 200);
  CHECK(log.at(2).epoch == 300);
}

TEST_CASE("wrap keeps the newest CAPACITY entries") {
  LiteEventLog log;
  for (uint32_t i = 0; i < LITE_EVENTLOG_CAPACITY + 5; i++) log.push(mk(i, 3));
  CHECK(log.count() == LITE_EVENTLOG_CAPACITY);
  // oldest should now be epoch 5 (first 5 evicted), newest = CAPACITY+4
  CHECK(log.at(0).epoch == 5);
  CHECK(log.at(LITE_EVENTLOG_CAPACITY - 1).epoch == (uint32_t)(LITE_EVENTLOG_CAPACITY + 4));
}

TEST_CASE("blob round-trip preserves order + fields") {
  LiteEventLog a;
  a.push(mk(10, 2));
  a.push(mk(20, 3));
  uint8_t blob[2 + 4 * sizeof(LiteEventLogEntry)];
  size_t n = a.toBlob(blob, sizeof(blob));
  CHECK(n == 2 + 2 * sizeof(LiteEventLogEntry));

  LiteEventLog b;
  CHECK(b.fromBlob(blob, n));
  CHECK(b.count() == 2);
  CHECK(b.at(0).epoch == 10);
  CHECK(b.at(1).epoch == 20);
  CHECK(b.at(1).energyWh == 40);
}

TEST_CASE("toBlob refuses an undersized buffer") {
  LiteEventLog a;
  a.push(mk(10, 2));
  uint8_t tiny[3];
  CHECK(a.toBlob(tiny, sizeof(tiny)) == 0);
}

TEST_CASE("fromBlob rejects bad version / truncation / overflow") {
  LiteEventLog a; a.push(mk(1, 2));
  uint8_t blob[2 + sizeof(LiteEventLogEntry)];
  a.toBlob(blob, sizeof(blob));

  LiteEventLog b;
  uint8_t bad = blob[0]; blob[0] = 99;            // wrong version
  CHECK_FALSE(b.fromBlob(blob, sizeof(blob)));
  CHECK(b.count() == 0);
  blob[0] = bad;

  CHECK_FALSE(b.fromBlob(blob, 1));               // too short for header
  CHECK_FALSE(b.fromBlob(blob, 2 + 1));           // claims 1 entry but body truncated

  uint8_t over[2]; over[0] = LITE_EVENTLOG_BLOB_VERSION; over[1] = 255; // count > capacity
  CHECK_FALSE(b.fromBlob(over, sizeof(over)));
}
