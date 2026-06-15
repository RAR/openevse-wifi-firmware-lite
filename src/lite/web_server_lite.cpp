#ifdef OPENEVSE_LITE
#include <Arduino.h>
#include <ArduinoJson.h>
#include <stdlib.h>
#include <WiFi.h>
#include <WebServer.h>
#include "uri/UriBraces.h"

#include "lite_evse_backend.h"
#include "lite_evse_manager.h"
#include "lite_clock.h"
#include "lite_energy_totals.h"
#include "manual.h"
#include "web_server_lite.h"
#include "espal_lite.h"
#include "lite_config_store.h"
#include "lite_charge_policy.h"
#include "lite_override.h"
#include "lite_schedule.h"
#include "lite_openevse_compat.h"
#include "lite_feed.h"
#include "lite_mqtt.h"
#include "lite_divert.h"
#include "lite_shaper.h"
#include "lite_provision.h"
#include "web_ui_lite.h"
#include "lwip/apps/sntp.h"

// Reported as OpenEVSE `firmware`/`version` so the HA integration shows a value.
#ifndef LITE_FW_VERSION
#define LITE_FW_VERSION "lite-web1"
#endif

// Manual override is defined in main_lite.cpp; reached here for /override + status.
extern ManualOverride manual;

// HTTP server over LibreTiny WiFiServer/WiFiClient = thread-safe lwIP sockets
// (no raw lwIP from the poll task; no 26 KB single mbuf). One client per
// handleClient() pass — fine for the single-client setup/dashboard.
static WebServer s_server(80);

// lwIP SNTP calls this (SNTP_SET_SYSTEM_TIME in lwipopts.h) with Unix seconds.
// Runs on the tcpip thread, so only stash — the value is applied to the clock
// from web_server_lite_loop() on the main task.
static volatile uint32_t s_sntpEpoch = 0;
static volatile bool     s_sntpHave  = false;
extern "C" void lite_sntp_set_system_time(unsigned int sec) {
  s_sntpEpoch = (uint32_t)sec;
  s_sntpHave  = true;
}

// Live LiteEvseManager handle stashed at begin() (the handler is a C-style callback
// and cannot capture, so a static pointer is how it reaches device state).
static LiteEvseManager *s_mgr_ctrl = NULL;

// Provisioning serving mode (set by the boot glue): selects which bundle GET /
// returns, and whether main_lite's loop drives the D3 STA-retry. Deferred-reboot
// state is queued by /connect and fired from web_server_lite_loop() after the
// HTTP response has flushed (never inside the handler before the socket drains).
static bool     s_apMode        = false;
static bool     s_rebootPending = false;
static uint32_t s_rebootAtMs    = 0;

void web_server_lite_set_ap_mode(bool ap) { s_apMode = ap; }
bool web_server_lite_in_ap_mode(void)     { return s_apMode; }

static LiteClock        *s_clock  = nullptr;
static LiteEnergyTotals *s_totals = nullptr;
static String            s_sntpHost = "pool.ntp.org";

static LiteDivertState s_divertState = { 0.0 };
static uint32_t        s_divertLastMs = 0;          // for smoothing delta_s
static uint32_t        s_divertMinChargeEndMs = 0;  // millis when min-charge window ends (0 = disarmed)
static bool            s_divertWasCharging = false;
static const uint32_t  FEED_STALE_MS = 120000;      // feed staleness window (fail-safe pause)

static LiteShaperState s_shaperState = { 0.0, false };
static uint32_t        s_shaperLastMs   = 0;  // smoothing delta_s
static uint32_t        s_shaperPauseMs  = 0;  // millis when current pause began (0 = not paused)
static const double    LITE_SHAPER_HYSTERESIS = 0.5; // A
static const int       LITE_MIN_CURRENT = 6;         // J1772 floor

// Active EVSE config cached in RAM so /status and GET /config never touch flash.
// Seeded at web_server_lite_begin() from the store (or defaults) and updated on POST.
static LiteEvseConfig s_cfg = { 32, 32 }; // {soft, hard} defaults (smallest JuiceBox sold)

// Solar-divert config cached in RAM. Seeded at begin() from the store (or upstream defaults).
static LiteDivertConfig s_divertCfg = { false, 0, 1.1, 20, 600, 600 }; // upstream defaults

// Load-shaper config cached in RAM. Seeded at begin() from the store (or upstream defaults).
static LiteShaperConfig s_shaperCfg = { false, 0, 60, 120, 300 }; // upstream defaults

// MQTT telemetry publisher + its config (loaded in begin, mutated by /config).
static LiteMqtt       s_mqtt;
static LiteMqttConfig s_mqttCfg;

// Pushed sensor feed (3c POST /status). Read by the divert/shaper glue below.
static LiteFeed s_feed;

// ---- /override (Slice 3b) volatile state (not persisted; resets on reboot) -------------
static LiteOverrideLimits s_ovrLimits;            // limits of the active override
static bool               s_ovrExpired  = false;  // a session limit fired -> sticky Disable
static bool               s_ovrEnabling = false;  // override resolves to Active (charging)
static bool               s_wasCharging = false;  // tracks the charge->idle falling edge

// ---- /schedule (Slice 4) -------------------------------------------------------------
static LiteSchedule s_schedule;            // persisted weekly schedule
static uint32_t     s_scheduleVersion = 0; // bumped on every mutation (exposed in /status)
static uint8_t      s_lastSchedState  = 0; // last schedule-resolved state applied (0/1/2)

static uint8_t sched_state_from_str(const char *s) {
  if (s && !strcmp(s, "active"))   return 1;
  if (s && !strcmp(s, "disabled")) return 2;
  return 0;
}
static const char *sched_state_str(uint8_t st) { return st == 1 ? "active" : "disabled"; }

// Serialize the whole schedule as a JSON array of {id,state,time,days}.
static void schedule_get_json(String &out) {
  // 16 events x {id,state,time,days[<=7]} -> ~2.1 KB worst case; size generously.
  StaticJsonDocument<2560> doc;
  JsonArray arr = doc.to<JsonArray>();
  for (uint32_t i = 0; i < s_schedule.count && i < LITE_SCHEDULE_MAX_EVENTS; i++) {
    const LiteScheduleEvent &e = s_schedule.events[i];
    JsonObject o = arr.createNestedObject();
    o["id"] = e.id;
    o["state"] = sched_state_str(e.state);
    char tb[12]; lite_schedule_format_time(e.sec_of_day, tb, sizeof(tb));
    o["time"] = tb;
    JsonArray days = o.createNestedArray("days");
    for (int d = 0; d < 7; d++) if (e.day_mask & (1u << d)) days.add(lite_schedule_day_name(d));
  }
  serializeJson(doc, out);
}

// Parse one event from a JSON body. Returns false (and leaves *code) on validation error.
static bool schedule_parse(const char *body, size_t len, LiteScheduleEvent &e, int &code) {
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, body, len) != DeserializationError::Ok) { code = 400; return false; }
  if (!doc.containsKey("state") || !doc.containsKey("time") || !doc.containsKey("days")) {
    code = 400; return false;
  }
  memset(&e, 0, sizeof(e));
  uint8_t st = sched_state_from_str(doc["state"]);
  if (st == 0) { code = 400; return false; }
  e.state = st;
  uint32_t sod;
  if (!lite_schedule_parse_time(doc["time"], sod)) { code = 400; return false; }
  e.sec_of_day = sod;
  uint8_t mask = 0;
  for (JsonVariant v : doc["days"].as<JsonArray>()) {
    int di = lite_schedule_day_index(v.as<const char *>());
    if (di < 0) { code = 400; return false; }
    mask |= (uint8_t)(1u << di);
  }
  if (mask == 0) { code = 400; return false; }
  e.day_mask = mask;
  // id: client-provided, else auto-assign max+1 (1 when empty).
  if (doc.containsKey("id") && (uint32_t)doc["id"] != 0) {
    e.id = (uint32_t)doc["id"];
  } else {
    uint32_t mx = 0;
    for (uint32_t i = 0; i < s_schedule.count && i < LITE_SCHEDULE_MAX_EVENTS; i++)
      if (s_schedule.events[i].id > mx) mx = s_schedule.events[i].id;
    e.id = mx + 1;
  }
  return true;
}

// GET (list) / POST (upsert) / DELETE?id= on the base path.
static void handle_schedule() {
  int code = 200; String body;
  if (s_server.method() == HTTP_POST) {
    String b = s_server.arg("plain");
    LiteScheduleEvent e;
    if (schedule_parse(b.c_str(), b.length(), e, code)) {
      if (lite_schedule_upsert(s_schedule, e)) {
        bool saved = lite_config_save_schedule(s_schedule); s_scheduleVersion++;
        code = saved ? 201 : 503;
        StaticJsonDocument<64> r; r["id"] = e.id; serializeJson(r, body);
      } else { code = 507; body = "{\"msg\":\"Schedule full\"}"; }
    } else { body = "{\"msg\":\"Bad schedule event\"}"; }
  } else if (s_server.method() == HTTP_DELETE) {
    uint32_t id = 0; String q = s_server.arg("id");
    if (q.length() > 0) id = (uint32_t)strtoul(q.c_str(), NULL, 10);
    if (id != 0 && lite_schedule_remove(s_schedule, id)) {
      lite_config_save_schedule(s_schedule); s_scheduleVersion++;
      body = "{\"msg\":\"Deleted\"}";
    } else { code = 404; body = "{\"msg\":\"Not found\"}"; }
  } else {
    schedule_get_json(body);   // GET
  }
  s_server.send(code, "application/json", body);
}

// DELETE /schedule/<id> (path form) — id from the UriBraces path arg.
static void handle_schedule_del_path() {
  int code = 200; String body;
  uint32_t id = (uint32_t)strtoul(s_server.pathArg(0).c_str(), NULL, 10);
  if (id != 0 && lite_schedule_remove(s_schedule, id)) {
    lite_config_save_schedule(s_schedule); s_scheduleVersion++;
    body = "{\"msg\":\"Deleted\"}";
  } else { code = 404; body = "{\"msg\":\"Not found\"}"; }
  s_server.send(code, "application/json", body);
}

static const char *override_state_str(EvseState s) {
  switch (s) {
    case EvseState::Active:   return "active";
    case EvseState::Disabled: return "disabled";
    default:                  return nullptr;       // None -> omit
  }
}

// Claim a parsed override; capture its limits + enabling flag; clear the expired latch.
static void override_apply(EvseProperties &props, const LiteOverrideLimits &lim) {
  s_ovrLimits   = lim;
  s_ovrExpired  = false;
  s_ovrEnabling = (props.getState() == EvseState::Active);
  manual.claim(props);
}

static void override_clear() {
  s_ovrLimits   = LiteOverrideLimits();
  s_ovrExpired  = false;
  s_ovrEnabling = false;
  manual.release();
}

// Parse a JSON override body into props + limits. False on JSON parse error.
static bool override_parse(const char *body, size_t len,
                           EvseProperties &props, LiteOverrideLimits &lim) {
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, body, len) != DeserializationError::Ok) return false;
  if (doc.containsKey("state")) {
    const char *s = doc["state"];
    if      (s && !strcmp(s, "active"))   props.setState(EvseState::Active);
    else if (s && !strcmp(s, "disabled")) props.setState(EvseState::Disabled);
    else if (s && !strcmp(s, "clear"))    props.setState(EvseState::None);
  }
  if (doc.containsKey("charge_current")) props.setChargeCurrent((uint32_t)doc["charge_current"]);
  if (doc.containsKey("max_current"))    props.setMaxCurrent((uint32_t)doc["max_current"]);
  if (doc.containsKey("auto_release"))   props.setAutoRelease((bool)doc["auto_release"]);
  if (doc.containsKey("energy_limit")) {
    lim.energy_limit_wh = (uint32_t)doc["energy_limit"]; lim.has_energy = true;
  }
  if (doc.containsKey("time_limit")) {
    lim.time_limit_s = (uint32_t)doc["time_limit"]; lim.has_time = true;
  }
  return true;
}

// Serialize the active override (or {}) into `out`.
static void override_get_json(String &out) {
  StaticJsonDocument<192> doc;
  if (manual.isActive()) {
    EvseProperties props;
    manual.getProperties(props);
    const char *st = override_state_str(props.getState());
    if (st) doc["state"] = st;
    if (props.hasChargeCurrent()) doc["charge_current"] = props.getChargeCurrent();
    if (props.hasMaxCurrent())    doc["max_current"]    = props.getMaxCurrent();
    doc["auto_release"] = props.isAutoRelease();
    if (s_ovrLimits.has_energy) doc["energy_limit"] = s_ovrLimits.energy_limit_wh;
    if (s_ovrLimits.has_time)   doc["time_limit"]   = s_ovrLimits.time_limit_s;
    doc["expired"] = s_ovrExpired;
  }
  serializeJson(doc, out);
}

static void handle_override() {
  int code = 200; String body;
  String qs = s_server.arg("state");   // legacy bodyless convenience (any method)
  if (qs.length() > 0) {
    if      (qs == "active")   { EvseProperties p(EvseState::Active);   LiteOverrideLimits l; override_apply(p, l); }
    else if (qs == "disabled") { EvseProperties p(EvseState::Disabled); LiteOverrideLimits l; override_apply(p, l); }
    else if (qs == "release" || qs == "clear") { override_clear(); }
    override_get_json(body);
  } else if (s_server.method() == HTTP_POST) {
    String b = s_server.arg("plain");
    EvseProperties props; LiteOverrideLimits lim;
    if (override_parse(b.c_str(), b.length(), props, lim)) {
      override_apply(props, lim); code = 201; body = "{\"msg\":\"Created\"}";
    } else { code = 400; body = "{\"msg\":\"Failed to parse JSON\"}"; }
  } else if (s_server.method() == HTTP_DELETE) {
    override_clear(); body = "{\"msg\":\"Deleted\"}";
  } else if (s_server.method() == HTTP_PATCH) {
    manual.toggle();
    s_ovrLimits = LiteOverrideLimits(); s_ovrExpired = false;
    EvseProperties tp; s_ovrEnabling = manual.getProperties(tp) && tp.getState() == EvseState::Active;
    body = "{\"msg\":\"Updated\"}";
  } else {
    override_get_json(body);   // GET
  }
  s_server.send(code, "application/json", body);
}

// Parse a POST /status JSON body into the pushed-feed store. Each key is optional
// (omit-when-absent): only present keys update. Mirrors the OpenEVSE handleStatusPost
// contract the firstof9 integration already sends.
static void status_post_apply(const char *body, size_t len)
{
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, body, len) != DeserializationError::Ok) return;
  uint32_t now = millis();
  if (doc.containsKey("solar"))           lite_feed_set_solar  (s_feed, doc["solar"].as<int>(),           now);
  if (doc.containsKey("grid_ie"))         lite_feed_set_grid_ie(s_feed, doc["grid_ie"].as<int>(),         now);
  if (doc.containsKey("voltage"))         lite_feed_set_voltage(s_feed, doc["voltage"].as<double>(),      now);
  if (doc.containsKey("shaper_live_pwr")) lite_feed_set_shaper (s_feed, doc["shaper_live_pwr"].as<int>(), now);
}

// Build the /status JSON in the OpenEVSE local-API shape the firstof9/openevse
// Home Assistant integration consumes (it polls this every 60 s). Keys it reads
// but a JuiceBox can't provide (divert/shaper/OCPP/GFCI counts/etc.) are simply
// omitted — the integration's .get() defaults tolerate absent keys.
void web_server_lite_build_status(JsonDocument &doc)
{
  if (s_mgr_ctrl) {
    LiteEvseState dev   = s_mgr_ctrl->getDeviceState();
    bool disabled       = (s_mgr_ctrl->getState() == EvseState::Disabled);

    // State (int + string), per the OpenEVSE contract.
    doc["state"]  = openevse_state_code(dev, disabled);
    doc["status"] = openevse_status_str(dev, disabled);

    // Live telemetry.
    int amp   = s_mgr_ctrl->getAmps();
    int power = s_mgr_ctrl->getPower();
    doc["amp"]               = amp;
    doc["pilot"]             = (uint32_t)s_mgr_ctrl->getChargeCurrent(); // advertised setpoint
    doc["power"]             = power;
    doc["tempt"]             = s_mgr_ctrl->getTemperature();
    doc["temp2"]             = s_mgr_ctrl->getTemperature();
    doc["max_current_soft"]  = (uint32_t)s_mgr_ctrl->getChargeCurrent();
    doc["max_current_hard"]  = s_mgr_ctrl->getMaxHardwareCurrent();
    doc["min_current_hard"]  = s_mgr_ctrl->getMinCurrent();
    doc["available_current"] = (uint32_t)s_mgr_ctrl->getMaxCurrent();
    doc["manual_override"]   = manual.isActive() ? 1 : 0;
    doc["mode"]              = "fast";

    // Derived voltage: power / amps while charging, else nominal 240 V. Keeps
    // HA's V x I ~= power consistent without a sensor the JuiceBox lacks.
    doc["voltage"] = (amp > 0 && power > 0) ? (power / amp) : 240;

    // Session energy (host-side accumulator).
    doc["wattsec"]        = (uint32_t)s_mgr_ctrl->getSessionWattSeconds();
    doc["watthour"]       = (uint32_t)s_mgr_ctrl->getSessionWattHours();
    doc["session_energy"] = (uint32_t)s_mgr_ctrl->getSessionWattHours();
    doc["elapsed"]        = (uint32_t)s_mgr_ctrl->getSessionElapsed();

    // Backend-specific extras (hw/fw/protocol/md/wc/wr/line + state_str). The
    // `wr` key carries the raw $WR fault string (the fault detail for state 8).
    s_mgr_ctrl->addStatusFields(doc);

    // Control/claim diagnostics (retained from the prior status body).
    doc["claims"] = (uint32_t)s_mgr_ctrl->activeClaimCount();
    doc["manual"] = manual.isActive() ? 1 : 0;
  }

  // Identity / system.
  doc["firmware"]  = LITE_FW_VERSION;
  doc["version"]   = LITE_FW_VERSION;
  { IPAddress ip = WiFi.localIP();
    char ipbuf[16];
    snprintf(ipbuf, sizeof(ipbuf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    doc["ipaddress"] = ipbuf; }
  doc["ssid"]      = WiFi.SSID();
  doc["srssi"]     = WiFi.RSSI();
  doc["free_heap"] = ESPAL.getFreeHeap();
  doc["freeram"]   = ESPAL.getFreeHeap();
  doc["uptime"]    = (uint32_t)(millis() / 1000);
  doc["reboot_reason"] = ESPAL.getRebootReason();   // RMU cause of the last reset (latched at boot)
  doc["schedule_version"] = s_scheduleVersion;

  if (s_clock && s_clock->valid()) {
    char isobuf[24];
    lite_clock_iso8601(s_clock->nowUtc(millis()), isobuf, sizeof(isobuf));
    doc["time"] = isobuf;                        // ISO-8601 UTC; omitted until first sync
  }
  if (s_totals) {
    doc["total_energy"]   = s_totals->lifetime_wh / 1000.0;  // kWh
    doc["total_day"]      = s_totals->day_wh    / 1000.0;
    doc["total_week"]     = s_totals->week_wh   / 1000.0;
    doc["total_month"]    = s_totals->month_wh  / 1000.0;
    doc["total_year"]     = s_totals->year_wh   / 1000.0;
    doc["total_switches"] = s_totals->switches;
  }

  // Echo the freshest pushed feed (3c) so dashboards / the HA integration can read it back.
  if (s_feed.solar_valid)   doc["solar"]           = s_feed.solar_w;
  if (s_feed.grid_valid)    doc["grid_ie"]         = s_feed.grid_ie_w;
  if (s_feed.voltage_valid) doc["voltage"]         = s_feed.voltage;
  if (s_feed.shaper_valid)  doc["shaper_live_pwr"] = s_feed.shaper_w;
}

static void build_status_json(String &out)
{
  StaticJsonDocument<1792> doc;   // bumped from 1280: field set grew (reboot_reason + rx-health);
                                  // 1280 was overflowing and silently dropping trailing fields
  web_server_lite_build_status(doc);
  serializeJson(doc, out);
}

// Serialize the cached config as the canonical /config response body.
static void config_json(String &out)
{
  StaticJsonDocument<1024> doc;   // headroom for long broker host/topic/user values
  doc["max_current_soft"] = s_cfg.max_current_soft;
  doc["max_current_hard"] = s_cfg.max_current_hard;
  doc["divert_enabled"]               = s_divertCfg.enabled;
  doc["divert_type"]                  = s_divertCfg.type;
  doc["divert_PV_ratio"]              = s_divertCfg.pv_ratio;
  doc["divert_attack_smoothing_time"] = s_divertCfg.attack_s;
  doc["divert_decay_smoothing_time"]  = s_divertCfg.decay_s;
  doc["divert_min_charge_time"]       = s_divertCfg.min_charge_s;
  doc["current_shaper_enabled"]          = s_shaperCfg.enabled;
  doc["current_shaper_max_pwr"]          = s_shaperCfg.max_pwr_w;
  doc["current_shaper_smoothing_time"]   = s_shaperCfg.smoothing_s;
  doc["current_shaper_data_maxinterval"] = s_shaperCfg.data_maxinterval_s;
  doc["current_shaper_min_pause_time"]   = s_shaperCfg.min_pause_s;
  doc["mqtt_enabled"] = s_mqttCfg.enabled;
  doc["mqtt_server"]  = s_mqttCfg.server;
  doc["mqtt_port"]    = s_mqttCfg.port;
  doc["mqtt_topic"]   = s_mqttCfg.topic;
  doc["mqtt_user"]    = s_mqttCfg.user;
  doc["mqtt_period"]  = s_mqttCfg.period_s;
  // mqtt_pass intentionally NOT echoed; a present non-empty mqtt_pass on POST replaces it.
  serializeJson(doc, out);
}

// Query arg present and non-empty (mirrors the old mg_get_http_var "> 0" gate).
static bool qarg(const char *k, String &v) { v = s_server.arg(k); return v.length() > 0; }

static void handle_config()
{
  String v;
  bool any = false;
  LiteEvseConfig cfg = s_cfg;
  if (qarg("max_current_hard", v)) { cfg.max_current_hard = v.toInt(); any = true; }
  if (qarg("max_current_soft", v)) { cfg.max_current_soft = v.toInt(); any = true; }

  if (qarg("tz_offset_min", v)) {
    LiteClockConfig cc; lite_config_load_clock(cc);
    cc.tz_offset_min = v.toInt(); lite_config_save_clock(cc);
    if (s_clock) s_clock->setTzOffsetMinutes(cc.tz_offset_min);
  }
  if (qarg("sntp_hostname", v)) {
    LiteClockConfig cc; lite_config_load_clock(cc);
    cc.sntp_hostname = v.c_str(); lite_config_save_clock(cc);
    s_sntpHost = v.c_str();
    sntp_setservername(0, s_sntpHost.c_str());
  }

  LiteDivertConfig dcfg = s_divertCfg; bool dany = false;
  if (qarg("divert_enabled", v)) { dcfg.enabled = v.toInt() != 0; dany = true; }
  if (qarg("divert_type", v))    { dcfg.type = v.toInt() ? 1 : 0; dany = true; }
  if (qarg("divert_PV_ratio", v))              { dcfg.pv_ratio = v.toFloat(); dany = true; }
  if (qarg("divert_attack_smoothing_time", v)) { dcfg.attack_s = (uint32_t)v.toInt(); dany = true; }
  if (qarg("divert_decay_smoothing_time", v))  { dcfg.decay_s = (uint32_t)v.toInt(); dany = true; }
  if (qarg("divert_min_charge_time", v))       { dcfg.min_charge_s = (uint32_t)v.toInt(); dany = true; }
  if (dany) { lite_config_save_divert(dcfg); s_divertCfg = dcfg; }

  LiteShaperConfig scfg = s_shaperCfg; bool sany = false;
  if (qarg("current_shaper_enabled", v)) { scfg.enabled = v.toInt() != 0; sany = true; }
  if (qarg("current_shaper_max_pwr", v))          { scfg.max_pwr_w = (uint32_t)v.toInt(); sany = true; }
  if (qarg("current_shaper_smoothing_time", v))   { scfg.smoothing_s = (uint32_t)v.toInt(); sany = true; }
  if (qarg("current_shaper_data_maxinterval", v)) { scfg.data_maxinterval_s = (uint32_t)v.toInt(); sany = true; }
  if (qarg("current_shaper_min_pause_time", v))   { scfg.min_pause_s = (uint32_t)v.toInt(); sany = true; }
  if (sany) { lite_config_save_shaper(scfg); s_shaperCfg = scfg; }

  LiteMqttConfig mcfg = s_mqttCfg; bool many = false;
  if (qarg("mqtt_enabled", v)) { mcfg.enabled = v.toInt() != 0; many = true; }
  if (qarg("mqtt_server", v))  { mcfg.server  = v;              many = true; }
  if (qarg("mqtt_port", v))    { int p = v.toInt(); mcfg.port = (p >= 1 && p <= 65535) ? p : 1883; many = true; }
  if (qarg("mqtt_topic", v))   { mcfg.topic   = v;              many = true; }
  if (qarg("mqtt_user", v))    { mcfg.user    = v;              many = true; }
  if (qarg("mqtt_period", v))  { mcfg.period_s = (uint32_t)v.toInt(); many = true; }
  // Password: only overwrite when a non-empty value is supplied (qarg requires
  // length > 0), so re-saving the form with a blank pass preserves the stored one.
  if (qarg("mqtt_pass", v))    { mcfg.pass    = v;              many = true; }
  if (many) { lite_config_save_mqtt(mcfg); s_mqttCfg = mcfg; s_mqtt.reconfigure(mcfg); }

  int status = 200;
  if (any) {
    cfg.max_current_hard = lite_clamp_service_max(cfg.max_current_hard);
    cfg.max_current_soft = lite_clamp_charge_current(cfg.max_current_soft, cfg.max_current_hard);
    bool saved = lite_config_save_evse(cfg);
    s_cfg = cfg;   // apply + cache even if persistence failed (best effort)
    if (s_mgr_ctrl) {
      s_mgr_ctrl->setTargetMaxCurrent((uint32_t)cfg.max_current_hard);
      s_mgr_ctrl->setTargetChargeCurrent((uint32_t)cfg.max_current_soft);
    }
    if (!saved) status = 503;   // applied but not persisted
  }

  String body; config_json(body);
  s_server.send(status, "application/json", body);
}

static void handle_status() {
  if (s_server.method() == HTTP_POST) {
    String b = s_server.arg("plain");
    status_post_apply(b.c_str(), b.length());
    s_server.send(200, "application/json", "{\"msg\":\"OK\"}");
  } else {
    String body; build_status_json(body);
    s_server.send(200, "application/json", body);
  }
}

// GET /scan -> [{ssid,rssi,enc}]. Synchronous scan; scanDelete() after (UAF-safe).
static void handle_scan() {
  int16_t n = WiFi.scanNetworks();
  StaticJsonDocument<3072> doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < n && i < 32; i++) {
    String ssid = WiFi.SSID((uint8_t)i);
    if (ssid.length() == 0) continue;   // omit hidden
    JsonObject o = arr.createNestedObject();
    o["ssid"] = ssid;
    o["rssi"] = WiFi.RSSI((uint8_t)i);
    o["enc"]  = lite_provision_enc((int)WiFi.encryptionType((uint8_t)i));
  }
  WiFi.scanDelete();
  String body; serializeJson(doc, body);
  s_server.send(200, "application/json", body);
}

// GET /connect?ssid=&pass= -> save creds, 200, then deferred reboot. WebServer
// URL-decodes args (Parsing.cpp), so store directly — no second decode.
static void handle_connect() {
  String ssid = s_server.arg("ssid");
  String pass = s_server.arg("pass");
  if (ssid.length() == 0) {
    s_server.send(400, "application/json", "{\"msg\":\"ssid required\"}");
    return;
  }
  LiteWifiConfig c;
  c.ssid = ssid.c_str();
  if (pass.length() > 0) c.pass = pass.c_str();
  if (!lite_config_save_wifi(c)) {
    s_server.send(500, "application/json", "{\"msg\":\"save failed\"}");
    return;
  }
  s_server.send(200, "application/json", "{\"msg\":\"OK\"}");
  s_rebootPending = true;
  s_rebootAtMs    = millis() + 750;   // reboot from loop() after the response flushes
}

// GET / -> gzip bundle for the current mode. Binary-safe: sendContent writes raw
// bytes (gzip has NULs). Sockets stream it (bounded by TCP_SND_BUF) — no big mbuf.
static void handle_root() {
  const uint8_t *body = s_apMode ? SETUP_HTML_GZ : INDEX_HTML_GZ;
  unsigned len        = s_apMode ? SETUP_HTML_GZ_LEN : INDEX_HTML_GZ_LEN;
  s_server.sendHeader("Content-Encoding", "gzip");
  s_server.setContentLength(len);
  s_server.send(200, "text/html", "");
  s_server.sendContent((const char *)body, (size_t)len);
}

static void handle_not_found() {
  s_server.send(404, "text/plain", "not found");
}

void web_server_lite_begin(LiteEvseManager &mgr, LiteClock &clock, LiteEnergyTotals &totals)
{
  s_mgr_ctrl = &mgr;
  s_clock    = &clock;
  s_totals   = &totals;

  LiteClockConfig cc;
  lite_config_load_clock(cc);
  s_sntpHost = cc.sntp_hostname;
  clock.setTzOffsetMinutes(cc.tz_offset_min);

  if (!lite_config_load_schedule(s_schedule)) {
    memset(&s_schedule, 0, sizeof(s_schedule));
  }

  // Load persisted config (or keep the 32/32 defaults), clamp, seed the manager target.
  if (!lite_config_load_evse(s_cfg)) {
    s_cfg = (LiteEvseConfig){ 32, 32 };
  }
  s_cfg.max_current_hard = lite_clamp_service_max(s_cfg.max_current_hard);
  s_cfg.max_current_soft = lite_clamp_charge_current(s_cfg.max_current_soft, s_cfg.max_current_hard);
  mgr.setTargetMaxCurrent((uint32_t)s_cfg.max_current_hard);
  mgr.setTargetChargeCurrent((uint32_t)s_cfg.max_current_soft);

  lite_config_load_divert(s_divertCfg);   // keeps defaults if the key is absent
  lite_config_load_shaper(s_shaperCfg);  // keeps defaults if the key is absent

  lite_config_load_mqtt(s_mqttCfg);       // fills defaults if absent (disabled by default)
  s_mqtt.begin(s_mqttCfg, ESPAL.getShortId(), LITE_FW_VERSION);

  s_server.on("/", handle_root);
  s_server.on("/status", handle_status);
  s_server.on("/config", handle_config);
  s_server.on("/override", handle_override);
  s_server.on("/schedule", handle_schedule);
  s_server.on(UriBraces("/schedule/{}"), HTTP_DELETE, handle_schedule_del_path);
  s_server.on("/scan", handle_scan);
  s_server.on("/connect", handle_connect);
  s_server.onNotFound(handle_not_found);
  s_server.begin();
  sntp_setoperatingmode(SNTP_OPMODE_POLL);
  sntp_setservername(0, s_sntpHost.c_str());   // s_sntpHost is a stable static String
  sntp_init();
}

void web_server_lite_loop()
{
  s_server.handleClient();
  if (s_sntpHave && s_clock) {
    s_sntpHave = false;
    s_clock->setEpoch((uint32_t)s_sntpEpoch, millis());
  }

  // Deferred post-/connect reboot: fires once the queued instant has passed, so
  // the {"msg":"OK"} response has been polled out before the radio resets.
  if (s_rebootPending && (int32_t)(millis() - s_rebootAtMs) >= 0) {
    ESPAL.reset();
  }

  // Slice 3b: enforce override session limits + auto-release. Pure decision in
  // lite_override_evaluate; this is the thin wiring to the manager seam.
  if (s_mgr_ctrl) {
    bool charging    = s_mgr_ctrl->isCharging();
    bool fallingEdge = (s_wasCharging && !charging);
    s_wasCharging    = charging;
    if (manual.isActive() && !s_ovrExpired) {
      EvseProperties cur; bool haveCur = manual.getProperties(cur);
      LiteOverrideAction act = lite_override_evaluate(
          s_ovrLimits,
          s_mgr_ctrl->getSessionWattHours(),
          s_mgr_ctrl->getSessionElapsed(),
          true, s_ovrEnabling, haveCur && cur.isAutoRelease(), fallingEdge);
      if (act == LiteOverrideAction::Stop) {
        EvseProperties p(EvseState::Disabled);
        manual.claim(p);
        s_ovrExpired = true; s_ovrEnabling = false;   // sticky: stays stopped until DELETE
      } else if (act == LiteOverrideAction::Release) {
        override_clear();
      }
    }
  }

  // Slice 4: time-of-day schedule. Resolve the active scheduled state from the local
  // wall-clock and claim it via the Schedule client (priority below a manual override).
  // Released while the clock is unsynced (no time basis). Re-claim only on change.
  if (s_mgr_ctrl && s_clock) {
    uint8_t st = 0;
    if (s_clock->valid()) {
      uint32_t local = s_clock->nowLocal(millis());
      int dow = (int)(((local / 86400u) + 4u) % 7u);  // 1970-01-01 = Thursday; Sun=0
      uint32_t sod = local % 86400u;
      st = lite_schedule_active_state(s_schedule, dow, sod);
    }
    if (st != s_lastSchedState) {
      if (st == 0) {
        s_mgr_ctrl->release(EvseClient_OpenEVSE_Schedule);
      } else {
        EvseProperties p(st == 1 ? EvseState::Active : EvseState::Disabled);
        s_mgr_ctrl->claim(EvseClient_OpenEVSE_Schedule, EvseManager_Priority_Timer, p);
      }
      s_lastSchedState = st;
    }
  }

  // Load-shaper: cap total current to the site-power budget via a MaxCurrent claim at
  // Safety (caps every client incl. manual). Pauses (Disabled@Limit) when the cap drops
  // below the min current or the shaper feed goes stale. Pure cap in lite_shaper_cap.
  if (s_mgr_ctrl && s_shaperCfg.enabled) {
    uint32_t now = millis();
    uint32_t delta_s = s_shaperLastMs ? (now - s_shaperLastMs) / 1000u : 0u;
    s_shaperLastMs = now;

    bool fresh = lite_feed_fresh(s_feed.shaper_valid, s_feed.shaper_ms, now,
                                 s_shaperCfg.data_maxinterval_s * 1000u);
    double volts = (s_feed.voltage_valid && s_feed.voltage > 1.0) ? s_feed.voltage : 240.0;

    if (!fresh) {
      // Stale feed -> pause charge (Disabled@Limit), arm pause timer, freeze smoothing.
      if (!s_shaperPauseMs) s_shaperPauseMs = now;
      s_shaperState.paused = true;
      if (s_mgr_ctrl->getState(EvseClient_OpenEVSE_Shaper) != EvseState::Disabled) {
        EvseProperties p(EvseState::Disabled);
        s_mgr_ctrl->claim(EvseClient_OpenEVSE_Shaper, EvseManager_Priority_Limit, p);
      }
    } else {
      int present_a = (int)s_mgr_ctrl->getChargeCurrent();
      double cap = lite_shaper_cap({ s_shaperCfg.max_pwr_w, s_shaperCfg.smoothing_s },
                                   s_shaperState, s_feed.shaper_w, volts, present_a,
                                   s_feed.solar_w, s_divertCfg.enabled && s_divertCfg.type == 0,
                                   delta_s);
      EvseProperties p;
      p.setMaxCurrent((uint32_t)(cap < 0 ? 0 : (uint32_t)cap));
      if (cap < LITE_MIN_CURRENT) {
        p.setState(EvseState::Disabled);            // not enough headroom -> pause
        if (!s_shaperPauseMs) s_shaperPauseMs = now;
        s_shaperState.paused = true;
      } else if (s_shaperPauseMs &&
                 (now - s_shaperPauseMs) >= s_shaperCfg.min_pause_s * 1000u &&
                 (cap - LITE_MIN_CURRENT) >= LITE_SHAPER_HYSTERESIS) {
        s_shaperPauseMs = 0;                        // resume after min-pause + hysteresis
        s_shaperState.paused = false;
      }
      s_mgr_ctrl->claim(EvseClient_OpenEVSE_Shaper, EvseManager_Priority_Safety, p);
    }
  } else if (s_mgr_ctrl && s_mgr_ctrl->clientHasClaim(EvseClient_OpenEVSE_Shaper)) {
    s_mgr_ctrl->release(EvseClient_OpenEVSE_Shaper);
    s_shaperState.smoothed_live_pwr = 0; s_shaperPauseMs = 0; s_shaperState.paused = false;
  }

  // Solar-divert (autonomous, OpenEVSE-priority). Claims Active@Divert(50) when excess
  // solar is sufficient; Disabled@Default(10) when not (so a schedule/manual still wins);
  // released entirely when divert is disabled. Pure decision in lite_divert_eval; feed
  // stale -> treat as 0 W (fail-safe wind-down).
  if (s_mgr_ctrl && s_divertCfg.enabled) {
    uint32_t now = millis();
    uint32_t delta_s = s_divertLastMs ? (now - s_divertLastMs) / 1000u : 0u;
    s_divertLastMs = now;

    bool fresh = (s_divertCfg.type == 0)
        ? lite_feed_fresh(s_feed.solar_valid, s_feed.solar_ms, now, FEED_STALE_MS)
        : lite_feed_fresh(s_feed.grid_valid,  s_feed.grid_ms,  now, FEED_STALE_MS);
    int    solar_w = fresh ? s_feed.solar_w   : 0;
    int    grid_w  = fresh ? s_feed.grid_ie_w : 0;
    double volts   = (s_feed.voltage_valid && s_feed.voltage > 1.0) ? s_feed.voltage : 240.0;

    bool active    = (s_mgr_ctrl->getState(EvseClient_OpenEVSE_Divert) == EvseState::Active);
    bool charging  = s_mgr_ctrl->isCharging();
    int  present_a = fresh ? (int)s_mgr_ctrl->getChargeCurrent() : 0;  // getAmps proxy; 0 on stale feed so GRID winds down (fail-safe)
    // "min-charge elapsed" (Stop permitted) when the car isn't actually charging OR the timer
    // expired — mirrors upstream getMinChargeTimeRemaining()==0 on (!isCharging() || timer done).
    bool min_elapsed = !charging
                     || (s_divertMinChargeEndMs == 0)
                     || ((int32_t)(now - s_divertMinChargeEndMs) >= 0);

    LiteDivertCfg dc { (LiteDivertType)s_divertCfg.type, s_divertCfg.pv_ratio,
                       s_divertCfg.attack_s, s_divertCfg.decay_s, 6 };
    LiteDivertResult r = lite_divert_eval(dc, s_divertState, solar_w, grid_w, volts,
                                          present_a, active, min_elapsed, delta_s);

    if (r.action == LiteDivertAction::Charge) {
      EvseProperties p(EvseState::Active);
      p.setChargeCurrent((uint32_t)r.charge_rate_a);
      s_mgr_ctrl->claim(EvseClient_OpenEVSE_Divert, EvseManager_Priority_Divert, p);
    } else if (r.action == LiteDivertAction::Stop) {
      EvseProperties p(EvseState::Disabled);
      s_mgr_ctrl->claim(EvseClient_OpenEVSE_Divert, EvseManager_Priority_Default, p);
    } // Hold: leave the existing claim untouched

    // Arm the min-charge timer on the rising edge of actual charging.
    if (charging && !s_divertWasCharging) {
      s_divertMinChargeEndMs = now + s_divertCfg.min_charge_s * 1000u;
      if (s_divertMinChargeEndMs == 0) s_divertMinChargeEndMs = 1; // avoid the disarmed sentinel
    }
    s_divertWasCharging = charging;
  } else if (s_mgr_ctrl && s_mgr_ctrl->clientHasClaim(EvseClient_OpenEVSE_Divert)) {
    s_mgr_ctrl->release(EvseClient_OpenEVSE_Divert);   // divert turned off -> drop the claim
    s_divertState.smoothed_available = 0;
  }

  // MQTT telemetry: per-field publish on cadence (faster while charging). No-op when
  // disabled or WiFi down. Reuses the /status doc builder so fields stay in sync.
  s_mqtt.loop(millis(), s_mgr_ctrl && s_mgr_ctrl->isCharging(),
              &web_server_lite_build_status);
}
#endif
