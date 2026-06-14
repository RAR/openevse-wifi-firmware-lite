#include "lite_openevse_compat.h"

int openevse_state_code(LiteEvseState s, bool controlDisabled) {
  if (s == LiteEvseState::Error) return 8;            // fault wins over control state
  if (controlDisabled)           return 254;          // sleeping
  switch (s) {
    case LiteEvseState::NotConnected: return 1;
    case LiteEvseState::Connected:    return 2;
    case LiteEvseState::Charging:     return 3;
    default:                          return 0;        // Unknown
  }
}

const char *openevse_status_str(LiteEvseState s, bool controlDisabled) {
  if (s == LiteEvseState::Error) return "error";
  if (controlDisabled)           return "sleeping";
  switch (s) {
    case LiteEvseState::NotConnected: return "not connected";
    case LiteEvseState::Connected:    return "connected";
    case LiteEvseState::Charging:     return "charging";
    default:                          return "unknown";
  }
}
