#pragma once
#include <stdint.h>
#include <string>

// Pure MQTT telemetry decision logic (no Arduino, no PubSubClient, no sockets) —
// native-testable. Time args are millis()-style uint32_t; deltas are wrap-safe.

// Publish cadence. period = charging ? chargeMs : idleMs.
//   lastMs == 0            -> true (never published yet)
//   (now - last) >= period -> true
//   else                   -> false
bool mqtt_should_publish(uint32_t lastMs, uint32_t nowMs,
                         uint32_t idleMs, uint32_t chargeMs, bool charging);

// Reconnect backoff. true when lastAttemptMs == 0 or (now - lastAttempt) >= backoffMs.
bool mqtt_backoff_due(uint32_t lastAttemptMs, uint32_t nowMs, uint32_t backoffMs);

// Join "<base>/<key>" with exactly one slash: trims trailing '/' from base and
// leading '/' from key. Empty base -> key; empty key -> base.
std::string mqtt_topic_join(const std::string &base, const std::string &key);

// Default base when configured topic is blank: "openevse-" + lowercase(shortId).
std::string mqtt_default_base(const std::string &shortId);
