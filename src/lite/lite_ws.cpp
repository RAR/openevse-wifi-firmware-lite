#include "lite_ws.h"

bool lite_ws_should_push(uint32_t nowMs, uint32_t lastPushMs, uint32_t intervalMs) {
  return (uint32_t)(nowMs - lastPushMs) >= intervalMs;
}
