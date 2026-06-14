#pragma once
#include <stdint.h>

// True when it's time to push again: (nowMs - lastPushMs) >= intervalMs, using unsigned
// subtraction so a 32-bit millis() wrap (~49.7 d) yields a correct delta. Pure.
bool lite_ws_should_push(uint32_t nowMs, uint32_t lastPushMs, uint32_t intervalMs);
