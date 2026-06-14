#ifdef OPENEVSE_LITE
// Minimal app_config shim for the lite build — self-contained (no ESP32 app_config.h).
//
// Historically the EVSE core reached into the full app_config.h for two inline
// helpers (config_default_state() / config_threephase_enabled()) that resolve
// against the global `flags` bitmask. The lite device build no longer compiles
// that core, so the only symbol it still exports is `flags` itself — provided
// here with the firmware default (CONFIG_DEFAULT_STATE set => EvseState::Active,
// CONFIG_THREEPHASE clear). Kept standalone so the lite firmware carries no
// dependency on the ESP32 configuration subsystem.
#include <stdint.h>

#ifndef CONFIG_DEFAULT_STATE
#define CONFIG_DEFAULT_STATE (1u << 26)   // matches upstream app_config bit 26
#endif

uint32_t flags = CONFIG_DEFAULT_STATE;

#endif // OPENEVSE_LITE
