#ifdef OPENEVSE_LITE
#include "lite_mqtt.h"
#include "lite_mqtt_policy.h"
#include <WiFi.h>

static const uint32_t LITE_MQTT_CHARGE_MS  = 5000;
static const uint32_t LITE_MQTT_BACKOFF_MS = 5000;
static const uint16_t LITE_MQTT_BUF        = 512;   // announce JSON exceeds the 256 default

void LiteMqtt::begin(const LiteMqttConfig &cfg, const String &shortId, const String &fwVersion)
{
  _cfg = cfg;
  _shortId = shortId;
  _fw = fwVersion;
  _mqtt.setBufferSize(LITE_MQTT_BUF);
  if (_cfg.server.length()) _mqtt.setServer(_cfg.server.c_str(), _cfg.port);
}

void LiteMqtt::reconfigure(const LiteMqttConfig &cfg)
{
  _cfg = cfg;
  if (_mqtt.connected()) _mqtt.disconnect();
  _lastAttempt = 0;   // allow an immediate reconnect with new settings
  if (_cfg.server.length()) _mqtt.setServer(_cfg.server.c_str(), _cfg.port);
}

bool LiteMqtt::connected() { return _mqtt.connected(); }

String LiteMqtt::baseTopic() const
{
  if (_cfg.topic.length()) return _cfg.topic;
  return String(mqtt_default_base(std::string(_shortId.c_str())).c_str());
}

bool LiteMqtt::connectNow()
{
  if (!_cfg.server.length()) return false;
  String clientId = String("juicebox-") + _shortId;
  String avail = String(mqtt_topic_join(std::string(baseTopic().c_str()), "available").c_str());

  bool ok;
  if (_cfg.user.length()) {
    ok = _mqtt.connect(clientId.c_str(), _cfg.user.c_str(), _cfg.pass.c_str(),
                       avail.c_str(), 0, true, "offline");
  } else {
    ok = _mqtt.connect(clientId.c_str(), nullptr, nullptr,
                       avail.c_str(), 0, true, "offline");
  }
  if (ok) {
    _mqtt.publish(avail.c_str(), "online", true);
    publishAnnounce();
  }
  return ok;
}

void LiteMqtt::publishAnnounce()
{
  StaticJsonDocument<192> a;
  a["firmware"] = _fw;
  a["id"]       = _shortId;
  IPAddress ip = WiFi.localIP();
  char ipbuf[16];
  snprintf(ipbuf, sizeof(ipbuf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  a["ipaddress"] = ipbuf;
  a["http"]      = String("http://") + ipbuf + "/";
  String out; serializeJson(a, out);
  String t = String(mqtt_topic_join(std::string(baseTopic().c_str()), "announce").c_str());
  _mqtt.publish(t.c_str(), out.c_str(), true);
}

void LiteMqtt::publishFields(JsonDocument &doc)
{
  std::string base = std::string(baseTopic().c_str());
  for (JsonPair kv : doc.as<JsonObject>()) {
    if (kv.value().is<JsonObject>() || kv.value().is<JsonArray>()) continue;  // scalars only
    std::string topic = mqtt_topic_join(base, std::string(kv.key().c_str()));
    String val = kv.value().as<String>();
    _mqtt.publish(topic.c_str(), val.c_str(), true);   // retained
  }
}

void LiteMqtt::loop(uint32_t nowMs, bool charging, void (*statusFn)(JsonDocument &))
{
  if (!_cfg.enabled) {
    if (_mqtt.connected()) _mqtt.disconnect();
    return;
  }
  if (WiFi.status() != WL_CONNECTED) return;

  if (!_mqtt.connected()) {
    if (!mqtt_backoff_due(_lastAttempt, nowMs, LITE_MQTT_BACKOFF_MS)) return;
    _lastAttempt = nowMs;
    if (!connectNow()) return;
    _lastPublish = 0;   // publish immediately after a fresh connect
  }

  _mqtt.loop();

  uint32_t idleMs = _cfg.period_s ? _cfg.period_s * 1000u : 30000u;
  if (mqtt_should_publish(_lastPublish, nowMs, idleMs, LITE_MQTT_CHARGE_MS, charging)) {
    _lastPublish = nowMs;
    StaticJsonDocument<1280> doc;
    statusFn(doc);
    publishFields(doc);
  }
}
#endif
