#pragma once
#ifdef OPENEVSE_LITE
#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFiClient.h>
#include <PubSubClient.h>
#include "lite_config_store.h"

// Outbound-only MQTT telemetry. Plaintext (WiFiClient). Per-field <base>/<key>
// publishes for OpenEVSE-MQTT parity. No subscriptions / control. Non-blocking:
// connect is attempted on a backoff inside loop(), which runs on the main task.
class LiteMqtt {
public:
  void begin(const LiteMqttConfig &cfg, const String &shortId, const String &fwVersion);
  void reconfigure(const LiteMqttConfig &cfg);   // apply live: disconnect, re-point server
  // Pump the client, reconnect on backoff, and publish per-field on cadence.
  // statusFn fills a JsonDocument with the fields to publish (pass
  // web_server_lite_build_status). No-op when disabled or WiFi is down.
  void loop(uint32_t nowMs, bool charging, void (*statusFn)(JsonDocument &));
  bool connected();
private:
  String baseTopic() const;       // cfg.topic or mqtt_default_base(shortId)
  bool   connectNow();            // single connect attempt + announce + availability
  void   publishAnnounce();
  void   publishFields(JsonDocument &doc);

  WiFiClient   _net;
  PubSubClient _mqtt{_net};
  LiteMqttConfig _cfg;
  String   _shortId;
  String   _fw;
  uint32_t _lastAttempt = 0;
  uint32_t _lastPublish = 0;
};
#endif
