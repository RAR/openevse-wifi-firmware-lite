#include "lite_clock.h"
#include <stdio.h>

void lite_civil_from_secs(uint32_t epoch, int &year, unsigned &month, unsigned &day) {
  int32_t  z   = (int32_t)(epoch / 86400u) + 719468;          // shift epoch to 0000-03-01
  int32_t  era = (z >= 0 ? z : z - 146096) / 146097;
  uint32_t doe = (uint32_t)(z - era * 146097);                // [0, 146096]
  uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; // [0, 399]
  int32_t  y   = (int32_t)yoe + era * 400;
  uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);     // [0, 365]
  uint32_t mp  = (5 * doy + 2) / 153;                          // [0, 11]
  day   = doy - (153 * mp + 2) / 5 + 1;                        // [1, 31]
  month = mp < 10 ? mp + 3 : mp - 9;                           // [1, 12]
  year  = y + (month <= 2);
}

void lite_clock_iso8601(uint32_t epoch, char *buf, size_t cap) {
  int y; unsigned mo, d;
  lite_civil_from_secs(epoch, y, mo, d);
  uint32_t sod = epoch % 86400u;                               // seconds of day
  unsigned hh = (unsigned)(sod / 3600u), mm = (unsigned)((sod % 3600u) / 60u), ss = (unsigned)(sod % 60u);
  snprintf(buf, cap, "%04d-%02u-%02uT%02u:%02u:%02uZ", y, mo, d, hh, mm, ss);
}
