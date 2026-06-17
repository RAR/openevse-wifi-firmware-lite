#pragma once
#ifdef OPENEVSE_LITE
#include <Arduino.h>
#include "lite_energy_totals.h"
#include "lite_schedule.h"

struct LiteWifiConfig { String ssid; String pass; };

// Typed EVSE config. Key names mirror upstream app_config so later module lifts
// find what they expect. Each field persists as its own FlashDB KV blob.
struct LiteEvseConfig {
  int max_current_soft; // active charge-current setpoint (A) the keepalive advertises
  int max_current_hard; // service-max ceiling (A) — install rating; soft is clamped to this
};

// Contract: call lite_config_begin() once at boot before any load/save/erase.
// Backed by a FlashDB KVDB on the `kvs` FAL partition (0x1F0000+0x8000).
bool lite_config_begin();                         // fdb_kvdb_init on the kvs partition; true on success

bool lite_config_load_wifi(LiteWifiConfig &out);  // false if no ssid stored yet
bool lite_config_save_wifi(const LiteWifiConfig &in);

bool lite_config_load_evse(LiteEvseConfig &out);  // false if max_current_hard key absent (use defaults)
bool lite_config_save_evse(const LiteEvseConfig &in);

// First-run setup gate. The web UI shows the onboarding wizard until /config
// reports wizard_passed:true, which the wizard's finish step POSTs. Persisted so
// onboarding doesn't reappear after reboot.
bool lite_config_load_wizard(bool &out);          // false if key absent (caller defaults to false)
bool lite_config_save_wizard(bool passed);

void lite_config_erase();                         // wipe WiFi creds (eraseConfig)

// Clock config (mirrors upstream keys). time_zone is the UI's "IANA|POSIX-TZ" string
// (kept for display round-trip); tz_offset_min is the standard UTC offset derived from it.
struct LiteClockConfig { String sntp_hostname; int tz_offset_min; String time_zone; bool sntp_enabled; };

bool lite_config_load_totals(LiteEnergyTotals &out);   // false if key absent (caller inits)
bool lite_config_save_totals(const LiteEnergyTotals &in);

bool lite_config_load_schedule(LiteSchedule &out);     // false if key absent (caller zero-inits)
bool lite_config_save_schedule(const LiteSchedule &in);

// Solar-divert config. Persisted as a single FlashDB blob ("divert").
struct LiteDivertConfig {
  bool     enabled;        // divert_enabled (de)
  int      type;           // divert_type (dm): 0=SOLAR, 1=GRID
  double   pv_ratio;       // divert_PV_ratio (dpr)
  uint32_t attack_s;       // divert_attack_smoothing_time (das)
  uint32_t decay_s;        // divert_decay_smoothing_time (dds)
  uint32_t min_charge_s;   // divert_min_charge_time (dt)
};
bool lite_config_load_divert(LiteDivertConfig &out);  // false if key absent (caller uses defaults)
bool lite_config_save_divert(const LiteDivertConfig &in);

// Load-shaper config. Persisted as a single FlashDB blob ("shaper").
struct LiteShaperConfig {
  bool     enabled;            // current_shaper_enabled (se)
  uint32_t max_pwr_w;          // current_shaper_max_pwr (smp)
  uint32_t smoothing_s;        // current_shaper_smoothing_time (sst)
  uint32_t data_maxinterval_s; // current_shaper_data_maxinterval (sdm)
  uint32_t min_pause_s;        // current_shaper_min_pause_time (spt)
};
bool lite_config_load_shaper(LiteShaperConfig &out);
bool lite_config_save_shaper(const LiteShaperConfig &in);

bool lite_config_load_clock(LiteClockConfig &out);     // fills defaults if keys absent
bool lite_config_save_clock(const LiteClockConfig &in);

// MQTT telemetry config. Persisted PER-KEY (mqtt_enabled/mqtt_server/mqtt_port/
// mqtt_topic/mqtt_user/mqtt_pass/mqtt_period) because String members can't go in a
// fdb_blob — same pattern as wifi/clock. Key names mirror upstream app_config.
struct LiteMqttConfig {
  bool     enabled;   // default false
  String   server;    // broker host/IP, default ""
  int      port;      // default 1883
  String   topic;     // base topic; "" -> mqtt_default_base(shortId) at runtime
  String   user;      // default ""
  String   pass;      // default ""
  uint32_t period_s;  // idle publish period seconds, default 30
};
bool lite_config_load_mqtt(LiteMqttConfig &out);  // fills defaults; true if any key present
bool lite_config_save_mqtt(const LiteMqttConfig &in);
#endif
