#include "lite_mqtt_policy.h"

bool mqtt_should_publish(uint32_t lastMs, uint32_t nowMs,
                         uint32_t idleMs, uint32_t chargeMs, bool charging)
{
  if (lastMs == 0) return true;
  uint32_t period = charging ? chargeMs : idleMs;
  return (uint32_t)(nowMs - lastMs) >= period;
}

bool mqtt_backoff_due(uint32_t lastAttemptMs, uint32_t nowMs, uint32_t backoffMs)
{
  if (lastAttemptMs == 0) return true;
  return (uint32_t)(nowMs - lastAttemptMs) >= backoffMs;
}

std::string mqtt_topic_join(const std::string &base, const std::string &key)
{
  std::string b = base, k = key;
  while (!b.empty() && b.back() == '/')  b.pop_back();
  size_t i = 0;
  while (i < k.size() && k[i] == '/')    ++i;
  k = k.substr(i);
  if (b.empty()) return k;
  if (k.empty()) return b;
  return b + "/" + k;
}

std::string mqtt_default_base(const std::string &shortId)
{
  std::string s = shortId;
  for (char &c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  return "openevse-" + s;
}
