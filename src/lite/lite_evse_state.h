#pragma once
#include <stdint.h>

// Backend-agnostic EVSE state. Each backend maps its native codes onto this.
enum class LiteEvseState : uint8_t {
  Unknown = 0,
  NotConnected,   // no vehicle on the pilot
  Connected,      // vehicle present, not charging
  Charging,
  Error,          // fault / GFI / lockout
};

// Human-readable canonical state name (stable strings for /status & UI).
inline const char *lite_evse_state_name(LiteEvseState s) {
  switch (s) {
    case LiteEvseState::NotConnected: return "not_connected";
    case LiteEvseState::Connected:    return "connected";
    case LiteEvseState::Charging:     return "charging";
    case LiteEvseState::Error:        return "error";
    default:                          return "unknown";
  }
}
