#include "lite_event_log.h"
#include <string.h>

void LiteEventLog::clear()
{
  _head = 0;
  _count = 0;
}

void LiteEventLog::push(const LiteEventLogEntry &e)
{
  if (_count < LITE_EVENTLOG_CAPACITY) {
    _buf[(_head + _count) % LITE_EVENTLOG_CAPACITY] = e;
    _count++;
  } else {
    // Full: overwrite the oldest and advance the head.
    _buf[_head] = e;
    _head = (_head + 1) % LITE_EVENTLOG_CAPACITY;
  }
}

const LiteEventLogEntry &LiteEventLog::at(size_t i) const
{
  // Caller guarantees i < _count; clamp defensively.
  if (i >= _count) i = _count ? _count - 1 : 0;
  return _buf[(_head + i) % LITE_EVENTLOG_CAPACITY];
}

size_t LiteEventLog::toBlob(void *buf, size_t cap) const
{
  size_t need = 2 + _count * sizeof(LiteEventLogEntry);
  if (cap < need) return 0;
  uint8_t *p = (uint8_t *)buf;
  p[0] = LITE_EVENTLOG_BLOB_VERSION;
  p[1] = (uint8_t)_count;
  for (size_t i = 0; i < _count; i++) {
    memcpy(p + 2 + i * sizeof(LiteEventLogEntry), &at(i), sizeof(LiteEventLogEntry));
  }
  return need;
}

bool LiteEventLog::fromBlob(const void *buf, size_t len)
{
  clear();
  if (len < 2) return false;
  const uint8_t *p = (const uint8_t *)buf;
  if (p[0] != LITE_EVENTLOG_BLOB_VERSION) return false;
  size_t n = p[1];
  if (n > LITE_EVENTLOG_CAPACITY) return false;
  if (len < 2 + n * sizeof(LiteEventLogEntry)) return false;
  for (size_t i = 0; i < n; i++) {
    LiteEventLogEntry e;
    memcpy(&e, p + 2 + i * sizeof(LiteEventLogEntry), sizeof(LiteEventLogEntry));
    push(e);
  }
  return true;
}
