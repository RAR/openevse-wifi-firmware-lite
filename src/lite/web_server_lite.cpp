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
#define LITE_FW_VERSION "lite-web2"
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
static bool              s_sntpEnabled = true;   // /config sntp_enabled (NTP on/off)
static String            s_timeZone    = "";     // /config time_zone ("IANA|POSIX") for UI round-trip

static LiteDivertState s_divertState = { 0.0 };
static uint32_t        s_divertLastMs = 0;          // for smoothing delta_s
static uint32_t        s_divertMinChargeEndMs = 0;  // millis when min-charge window ends (0 = disarmed)
static bool            s_divertWasCharging = false;
static const uint32_t  FEED_STALE_MS = 120000;      // feed staleness window (fail-safe pause)

static LiteShaperState s_shaperState = { 0.0, false };
// Runtime divert/shaper results cached for /status (the Solar/Shaper pages read them live).
static int    s_divertChargeRate = 0;    // last divert-commanded charge current (A)
static double s_shaperCur        = 0.0;  // last shaper current cap (A)
static bool   s_shaperUpdated    = false;// shaper produced a fresh cap this cycle
static uint32_t        s_shaperLastMs   = 0;  // smoothing delta_s
static uint32_t        s_shaperPauseMs  = 0;  // millis when current pause began (0 = not paused)
static const double    LITE_SHAPER_HYSTERESIS = 0.5; // A
static const int       LITE_MIN_CURRENT = 6;         // J1772 floor

// Active EVSE config cached in RAM so /status and GET /config never touch flash.
// Seeded at web_server_lite_begin() from the store (or defaults) and updated on POST.
static LiteEvseConfig s_cfg = { 32, 32 }; // {soft, hard} defaults (smallest JuiceBox sold)
static bool s_wizardPassed = false;       // first-run setup gate the web UI watches (config.wizard_passed)

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
    int state_code = openevse_state_code(dev, disabled);
    doc["state"]   = state_code;
    doc["status"]  = openevse_status_str(dev, disabled);
    doc["vehicle"] = (state_code == 2 || state_code == 3) ? 1 : 0;  // plugged in (connected/charging)

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
    bool evse_online = doc["comms_online"].as<bool>();
    doc["evse_connected"] = evse_online ? 1 : 0;   // UI "EVSE not connected" banner keys off this
    doc["rapi_connected"] = evse_online ? 1 : 0;

    // Telemetry under the canonical names the web UI reads.
    doc["temp"]            = s_mgr_ctrl->getTemperature();          // dashboard temp chip reads .temp
    doc["session_elapsed"] = (uint32_t)s_mgr_ctrl->getSessionElapsed();
    doc["max_current"]     = s_mgr_ctrl->getMaxHardwareCurrent();
    doc["service_level"]   = 1;

    // Eco/divert + load-shaper runtime (the Solar/Shaper settings pages show these live).
    doc["divertmode"]     = s_divertCfg.enabled ? 2 : 1;   // 2=Eco(divert) selected, 1=normal
    doc["divert_active"]  = (s_mgr_ctrl->getState(EvseClient_OpenEVSE_Divert) == EvseState::Active) ? 1 : 0;
    doc["charge_rate"]    = s_divertChargeRate;
    doc["shaper"]         = s_shaperCfg.enabled ? 1 : 0;
    doc["shaper_cur"]     = s_shaperCur;
    doc["shaper_updated"] = s_shaperUpdated;

    // Control/claim diagnostics (retained from the prior status body).
    doc["claims"] = (uint32_t)s_mgr_ctrl->activeClaimCount();
    doc["manual"] = manual.isActive() ? 1 : 0;
  }

  // Identity / system.
  doc["firmware"]  = LITE_FW_VERSION;
  doc["version"]   = LITE_FW_VERSION;
  doc["ota_bank"]  = lt_ota_dual_get_current();   // 1 = bank A (@0x008000), 2 = bank B (@0x100000)
  { IPAddress ip = WiFi.localIP();
    char ipbuf[16];
    snprintf(ipbuf, sizeof(ipbuf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    doc["ipaddress"] = ipbuf; }
  doc["ssid"]      = WiFi.SSID();
  doc["srssi"]     = WiFi.RSSI();
  doc["mode"]                  = s_apMode ? "AP" : "STA";  // NETWORK mode (UI Network card), not charge mode
  doc["wifi_client_connected"] = (WiFi.status() == WL_CONNECTED) ? 1 : 0;  // UI "Connection" row
  doc["eth_connected"]         = 0;                        // no wired NIC on the WGM160P
  doc["net_connected"]         = (WiFi.status() == WL_CONNECTED) ? 1 : 0;
  doc["macaddress"]            = WiFi.macAddress();
  doc["free_heap"] = ESPAL.getFreeHeap();
  doc["freeram"]   = ESPAL.getFreeHeap();
  doc["uptime"]    = (uint32_t)(millis() / 1000);
  doc["reboot_reason"] = ESPAL.getRebootReason();   // RMU cause of the last reset (latched at boot)
  doc["schedule_version"] = s_scheduleVersion;
  // Store-version counters the UI watches to refresh on change. lite only tracks schedule;
  // the rest are 0 (the UI fetches them once at startup and trusts optimistic local updates
  // on its own writes), present so the UI reads defined values rather than undefined.
  doc["config_version"]        = 0;
  doc["claims_version"]        = 0;
  doc["override_version"]      = 0;
  doc["schedule_plan_version"] = 0;
  doc["limit_version"]         = 0;
  // Subsystem connection states. mqtt is real on lite; the rest are subsystems a JuiceBox
  // lacks -> honest "not connected" defaults so the UI shows status instead of blanks.
  doc["mqtt_connected"]    = s_mqtt.connected() ? 1 : 0;
  doc["emoncms_connected"] = 0;
  doc["ocpp_connected"]    = 0;
  doc["rfid_failure"]      = 0;
  doc["ohm_hour"]          = "NotConnected";
  doc["packets_sent"]      = 0;
  doc["packets_success"]   = 0;
  // Fault/diagnostic counters + flags the JuiceBox host doesn't surface.
  doc["gfcicount"]  = 0;
  doc["nogndcount"] = 0;
  doc["stuckcount"] = 0;
  doc["flags"]      = 0;
  doc["colour"]     = 0;
  doc["ota_update"] = 0;
  doc["limit"]      = false;

  if (s_clock && s_clock->valid()) {
    char isobuf[24];
    lite_clock_iso8601(s_clock->nowUtc(millis()), isobuf, sizeof(isobuf));
    doc["time"] = isobuf;                        // ISO-8601 UTC; omitted until first sync
    // local_time (offset-suffixed) + offset string, for the UI clock display.
    char localbuf[24];
    lite_clock_iso8601(s_clock->nowLocal(millis()), localbuf, sizeof(localbuf));
    size_t ll = strlen(localbuf);
    if (ll && localbuf[ll - 1] == 'Z') localbuf[ll - 1] = '\0';   // strip the UTC 'Z'
    int off = s_clock->tzOffsetMinutes();
    char offbuf[8];
    snprintf(offbuf, sizeof(offbuf), "%c%02d%02d", off < 0 ? '-' : '+', abs(off) / 60, abs(off) % 60);
    doc["local_time"] = String(localbuf) + offbuf;
    doc["offset"]     = offbuf;
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
  StaticJsonDocument<3072> doc;   // bumped 1280->1792->3072 as the field set grew (canonical
                                  // /status alignment); undersizing silently drops trailing fields
  web_server_lite_build_status(doc);
  serializeJson(doc, out);
}

// --- POSIX TZ parsing (the UI's time_zone is an "IANA|POSIX-TZ" string) ----------
// e.g. "EST5EDT,M3.2.0,M11.1.0", "CET-1CEST,M3.5.0,M10.5.0/3", "<+0530>-5:30".

// Days since 1970-01-01 for civil Y/M/D (Howard Hinnant's algorithm).
static long tz_days_from_civil(int y, int m, int d) {
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  int  yoe = (int)(y - era * 400);
  int  doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int  doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097L + doe - 719468;
}

// Civil year that contains a given day count.
static int tz_year_from_days(long z) {
  z += 719468;
  long era = (z >= 0 ? z : z - 146096) / 146097;
  int  doe = (int)(z - era * 146097);
  int  yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int  y   = yoe + (int)(era * 400);
  int  doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  int  mp  = (5 * doy + 2) / 153;
  int  m   = mp + (mp < 10 ? 3 : -9);
  return y + (m <= 2);
}

// Parse a transition rule "Mm.w.d[/h[:mm[:ss]]]" at *p (advances p). Only the M form
// is supported — it covers every zone in the UI's zones.json. time_s defaults to 02:00.
static bool tz_parse_rule(const char *&p, int &mon, int &wk, int &dow, int &time_s) {
  time_s = 2 * 3600;
  if (*p != 'M') return false;
  p++;
  mon = 0; while (*p >= '0' && *p <= '9') { mon = mon * 10 + (*p - '0'); p++; }
  if (*p != '.') return false; p++;
  wk  = 0; while (*p >= '0' && *p <= '9') { wk  = wk  * 10 + (*p - '0'); p++; }
  if (*p != '.') return false; p++;
  dow = 0; while (*p >= '0' && *p <= '9') { dow = dow * 10 + (*p - '0'); p++; }
  if (*p == '/') {
    p++; int hh = 0; while (*p >= '0' && *p <= '9') { hh = hh * 10 + (*p - '0'); p++; }
    int mm = 0; if (*p == ':') { p++; while (*p >= '0' && *p <= '9') { mm = mm * 10 + (*p - '0'); p++; }
                                 if (*p == ':') { p++; while (*p >= '0' && *p <= '9') p++; } }
    time_s = hh * 3600 + mm * 60;
  }
  return true;
}

// Civil-time seconds (local wall clock, treated as if UTC) of an "Mm.w.d" rule in year Y.
static long tz_rule_civil_secs(int Y, int mon, int wk, int dow, int time_s) {
  long first = tz_days_from_civil(Y, mon, 1);
  int  first_dow = (int)(((first % 7) + 4 + 7) % 7);   // 1970-01-01 = Thu(4); 0 = Sunday
  int  day = 1 + ((dow - first_dow + 7) % 7) + (wk - 1) * 7;
  static const int mdays[] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
  int dim = mdays[mon - 1];
  if (mon == 2 && ((Y % 4 == 0 && Y % 100 != 0) || Y % 400 == 0)) dim = 29;
  if (day > dim) day -= 7;                              // wk == 5 -> last occurrence
  return tz_days_from_civil(Y, mon, day) * 86400L + time_s;
}

// Skip a TZ designation (<...> or letters) then a [+-]hh[:mm[:ss]] offset at *p.
static void tz_skip_name_offset(const char *&p) {
  if (*p == '<') { while (*p && *p != '>') p++; if (*p) p++; }
  else { while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) p++; }
  if (*p == '+' || *p == '-') p++;
  while (*p >= '0' && *p <= '9') p++;
  if (*p == ':') { p++; while (*p >= '0' && *p <= '9') p++;
                   if (*p == ':') { p++; while (*p >= '0' && *p <= '9') p++; } }
}

// Standard UTC offset (minutes EAST of UTC) from a POSIX TZ. The numeric field is hours
// WEST of UTC, so we negate it: "EST5..." -> -300, "CET-1..." -> +60, "GMT0" -> 0.
static int posix_tz_offset_min(const char *tz) {
  if (!tz || !*tz) return 0;
  const char *p = tz;
  if (*p == '<') { while (*p && *p != '>') p++; if (*p == '>') p++; }
  else { while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) p++; }
  int sign = 1;
  if      (*p == '+') p++;
  else if (*p == '-') { sign = -1; p++; }
  if (!(*p >= '0' && *p <= '9')) return 0;
  int hh = 0; while (*p >= '0' && *p <= '9') { hh = hh * 10 + (*p - '0'); p++; }
  int mm = 0;
  if (*p == ':') { p++; while (*p >= '0' && *p <= '9') { mm = mm * 10 + (*p - '0'); p++; } }
  return -(sign * (hh * 60 + mm));
}

// Effective UTC offset (minutes east) for `utc`, honoring the POSIX DST rules when present.
// Falls back to the standard offset when there's no DST clause or an unsupported rule form.
static int lite_tz_offset_min(const char *tz, uint32_t utc) {
  int std_off = posix_tz_offset_min(tz);
  if (!tz || !*tz) return std_off;
  const char *p = tz;
  tz_skip_name_offset(p);                  // past the std name + offset
  if (!*p || *p == ',') return std_off;    // no DST designation -> no DST
  if (*p == '<') { while (*p && *p != '>') p++; if (*p) p++; }   // DST name
  else { while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) p++; }
  int dst_off = std_off + 60;              // POSIX default: 1h ahead of std
  if (*p == '+' || *p == '-' || (*p >= '0' && *p <= '9')) {
    int sign = 1; if (*p == '+') p++; else if (*p == '-') { sign = -1; p++; }
    int hh = 0; while (*p >= '0' && *p <= '9') { hh = hh * 10 + (*p - '0'); p++; }
    int mm = 0; if (*p == ':') { p++; while (*p >= '0' && *p <= '9') { mm = mm * 10 + (*p - '0'); p++; }
                                 if (*p == ':') { p++; while (*p >= '0' && *p <= '9') p++; } }
    dst_off = -(sign * (hh * 60 + mm));
  }
  if (*p != ',') return std_off;           // DST name but no rules -> can't place the window
  p++;
  int sm, sw, sdow, sts, em, ew, edow, ets;
  if (!tz_parse_rule(p, sm, sw, sdow, sts)) return std_off;
  if (*p != ',') return std_off; p++;
  if (!tz_parse_rule(p, em, ew, edow, ets)) return std_off;

  int  Y = tz_year_from_days((long)utc / 86400);
  long start_utc = tz_rule_civil_secs(Y, sm, sw, sdow, sts) - (long)std_off * 60;  // switch at local std
  long end_utc   = tz_rule_civil_secs(Y, em, ew, edow, ets) - (long)dst_off * 60;  // switch at local dst
  long now = (long)utc;
  bool in_dst = (start_utc < end_utc) ? (now >= start_utc && now < end_utc)        // N. hemisphere
                                      : (now >= start_utc || now < end_utc);       // S. hemisphere
  return in_dst ? dst_off : std_off;
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
  doc["wizard_passed"] = s_wizardPassed;   // first-run gate; App.svelte shows the wizard until true
  // Firmware versions for the UI's About/Firmware pages, which read them from
  // /config (not /status): "version" = WiFi gateway (this lite fw), "firmware" =
  // EVSE controller. lite has no validated controller-fw read, so both report
  // LITE_FW_VERSION (matches /status). GUI version is the UI's build-time const.
  doc["firmware"] = LITE_FW_VERSION;
  doc["version"]  = LITE_FW_VERSION;
  doc["ssid"]     = WiFi.SSID();   // UI Network card SSID row reads config.ssid
  // Time settings (UI Time page reads these from /config).
  doc["time_zone"]     = s_timeZone;
  doc["sntp_enabled"]  = s_sntpEnabled;
  doc["sntp_hostname"] = s_sntpHost;
  serializeJson(doc, out);
}

// Read a config param from a POST: the web UI sends a JSON object of changed
// keys (Content-Type application/json); legacy/curl callers may send form args.
// `json` selects the source; value (numbers/bools stringified) returns in v so
// the per-field toInt()/toFloat() apply logic below is shared by both paths.
static bool cfg_get(JsonDocument &doc, bool json, const char *k, String &v) {
  if (json) {
    if (!doc.containsKey(k)) return false;
    JsonVariantConst jv = doc[k];
    if      (jv.is<bool>())         v = jv.as<bool>() ? "1" : "0";   // downstream does toInt()!=0
    else if (jv.is<const char *>()) v = jv.as<const char *>();
    else if (jv.is<float>())        v = String(jv.as<double>(), 6);  // covers JSON ints + floats in v6
    else                            v = jv.as<String>();
    return true;
  }
  v = s_server.arg(k);
  return v.length() > 0;
}

static void handle_config()
{
  // GET -> the full config object.
  if (s_server.method() == HTTP_GET) {
    String body; config_json(body);
    s_server.send(200, "application/json", body);
    return;
  }

  // POST: parse the raw body (arg "plain") as JSON; fall back to form args when
  // it isn't valid JSON. The web UI's config_store.upload() POSTs JSON and keys
  // success off a {"msg":"done"|"no change"} reply.
  StaticJsonDocument<1024> doc;
  bool json = false;
  {
    String plain = s_server.arg("plain");
    if (plain.length() && deserializeJson(doc, plain) == DeserializationError::Ok && doc.is<JsonObject>())
      json = true;
  }

  String v;
  bool changed = false;   // anything applied -> {"msg":"done"} (else "no change")
  int  status  = 200;

  LiteEvseConfig cfg = s_cfg; bool any = false;
  if (cfg_get(doc, json, "max_current_hard", v)) { cfg.max_current_hard = v.toInt(); any = true; }
  if (cfg_get(doc, json, "max_current_soft", v)) { cfg.max_current_soft = v.toInt(); any = true; }

  if (cfg_get(doc, json, "tz_offset_min", v)) {
    LiteClockConfig cc; lite_config_load_clock(cc);
    cc.tz_offset_min = v.toInt(); lite_config_save_clock(cc);
    if (s_clock) s_clock->setTzOffsetMinutes(cc.tz_offset_min);
    changed = true;
  }
  if (cfg_get(doc, json, "sntp_hostname", v)) {
    LiteClockConfig cc; lite_config_load_clock(cc);
    cc.sntp_hostname = v.c_str(); lite_config_save_clock(cc);
    s_sntpHost = v.c_str();
    sntp_setservername(0, s_sntpHost.c_str());
    changed = true;
  }
  if (cfg_get(doc, json, "time_zone", v)) {
    // The UI value is "IANA|POSIX-TZ". Apply the EFFECTIVE (DST-aware) offset right
    // away when the clock is synced so the change shows immediately (the loop's
    // periodic re-check then handles the twice-yearly DST flip); fall back to the
    // standard offset before first time sync.
    int bar = v.indexOf('|');
    String posix = (bar >= 0) ? v.substring(bar + 1) : v;
    int off = (s_clock && s_clock->valid())
                ? lite_tz_offset_min(posix.c_str(), s_clock->nowUtc(millis()))
                : posix_tz_offset_min(posix.c_str());
    LiteClockConfig cc; lite_config_load_clock(cc);
    cc.time_zone = v.c_str(); cc.tz_offset_min = off; lite_config_save_clock(cc);
    s_timeZone = v.c_str();
    if (s_clock) s_clock->setTzOffsetMinutes(off);
    changed = true;
  }
  if (cfg_get(doc, json, "sntp_enabled", v)) {
    bool en = v.toInt() != 0;
    LiteClockConfig cc; lite_config_load_clock(cc);
    cc.sntp_enabled = en; lite_config_save_clock(cc);
    s_sntpEnabled = en;
    if (en) { sntp_setservername(0, s_sntpHost.c_str()); sntp_init(); }
    else    { sntp_stop(); }   // manual mode: stop NTP so it can't override a set clock
    changed = true;
  }

  LiteDivertConfig dcfg = s_divertCfg; bool dany = false;
  if (cfg_get(doc, json, "divert_enabled", v)) { dcfg.enabled = v.toInt() != 0; dany = true; }
  if (cfg_get(doc, json, "divert_type", v))    { dcfg.type = v.toInt() ? 1 : 0; dany = true; }
  if (cfg_get(doc, json, "divert_PV_ratio", v))              { dcfg.pv_ratio = v.toFloat(); dany = true; }
  if (cfg_get(doc, json, "divert_attack_smoothing_time", v)) { dcfg.attack_s = (uint32_t)v.toInt(); dany = true; }
  if (cfg_get(doc, json, "divert_decay_smoothing_time", v))  { dcfg.decay_s = (uint32_t)v.toInt(); dany = true; }
  if (cfg_get(doc, json, "divert_min_charge_time", v))       { dcfg.min_charge_s = (uint32_t)v.toInt(); dany = true; }
  if (dany) { lite_config_save_divert(dcfg); s_divertCfg = dcfg; changed = true; }

  LiteShaperConfig scfg = s_shaperCfg; bool sany = false;
  if (cfg_get(doc, json, "current_shaper_enabled", v)) { scfg.enabled = v.toInt() != 0; sany = true; }
  if (cfg_get(doc, json, "current_shaper_max_pwr", v))          { scfg.max_pwr_w = (uint32_t)v.toInt(); sany = true; }
  if (cfg_get(doc, json, "current_shaper_smoothing_time", v))   { scfg.smoothing_s = (uint32_t)v.toInt(); sany = true; }
  if (cfg_get(doc, json, "current_shaper_data_maxinterval", v)) { scfg.data_maxinterval_s = (uint32_t)v.toInt(); sany = true; }
  if (cfg_get(doc, json, "current_shaper_min_pause_time", v))   { scfg.min_pause_s = (uint32_t)v.toInt(); sany = true; }
  if (sany) { lite_config_save_shaper(scfg); s_shaperCfg = scfg; changed = true; }

  LiteMqttConfig mcfg = s_mqttCfg; bool many = false;
  if (cfg_get(doc, json, "mqtt_enabled", v)) { mcfg.enabled = v.toInt() != 0; many = true; }
  if (cfg_get(doc, json, "mqtt_server", v))  { mcfg.server  = v;              many = true; }
  if (cfg_get(doc, json, "mqtt_port", v))    { int p = v.toInt(); mcfg.port = (p >= 1 && p <= 65535) ? p : 1883; many = true; }
  if (cfg_get(doc, json, "mqtt_topic", v))   { mcfg.topic   = v;              many = true; }
  if (cfg_get(doc, json, "mqtt_user", v))    { mcfg.user    = v;              many = true; }
  if (cfg_get(doc, json, "mqtt_period", v))  { mcfg.period_s = (uint32_t)v.toInt(); many = true; }
  // Password: only overwrite when a non-empty value is supplied (cfg_get requires
  // length > 0 on the form path), so re-saving with a blank pass keeps the stored one.
  if (cfg_get(doc, json, "mqtt_pass", v) && v.length()) { mcfg.pass = v;       many = true; }
  if (many) { lite_config_save_mqtt(mcfg); s_mqttCfg = mcfg; s_mqtt.reconfigure(mcfg); changed = true; }

  // First-run setup gate: the web UI's wizard finish POSTs wizard_passed:true.
  if (cfg_get(doc, json, "wizard_passed", v)) {
    bool wp = v.toInt() != 0;
    if (wp != s_wizardPassed) { s_wizardPassed = wp; lite_config_save_wizard(wp); }
    changed = true;
  }

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
    changed = true;
  }

  StaticJsonDocument<48> r;
  r["msg"] = changed ? "done" : "no change";
  String resp; serializeJson(r, resp);
  s_server.send(status, "application/json", resp);
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

// POST /update — OTA firmware upload. Streams the dual-bank .uf2 bytes straight into
// the LibreTiny uf2ota engine via the low-level lt_ota_* API (NOT the Arduino Update
// wrapper: Update.cpp pulls an MD5 impl that silabs-efm32gg11 deliberately disables —
// LT_ARD_MD5_MBEDTLS 0, no mbedtls md5 in this core — and we don't use Update's optional
// MD5 check anyway). uf2ota parses the UF2 blocks, writes the INACTIVE bank, and on
// lt_ota_end() activates it as a TRIAL. On success we schedule the shared deferred reboot
// so the JSON reply flushes first; the new image self-confirms from loop() once it's live
// (lite_ota_should_confirm), else the bootloader auto-reverts after 3 boots. No auth,
// matching every other lite endpoint.
static bool          s_otaOk  = false;
static String        s_otaErr;
static lt_ota_ctx_t *s_otaCtx = nullptr;
static void handle_update_upload() {
  HTTPUpload &up = s_server.upload();
  if (up.status == UPLOAD_FILE_START) {
    s_otaErr = ""; s_otaOk = false;
    if (s_otaCtx) { lt_ota_end(s_otaCtx); free(s_otaCtx); s_otaCtx = nullptr; }   // stale ctx
    s_otaCtx = (lt_ota_ctx_t *)malloc(sizeof(lt_ota_ctx_t));
    if (!s_otaCtx) { s_otaErr = "alloc"; return; }
    lt_ota_begin(s_otaCtx, 0);   // size unknown — UF2 blocks are self-delimiting
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (s_otaCtx && s_otaErr.length() == 0 &&
        lt_ota_write(s_otaCtx, up.buf, up.currentSize) != up.currentSize) {
      s_otaErr = String("uf2 write err ") + (int)s_otaCtx->error;
    }
  } else if (up.status == UPLOAD_FILE_END) {
    if (s_otaCtx && s_otaErr.length() == 0) {
      // Success requires BOTH a clean end AND that a real, complete image was written:
      // bytes_written>0 rejects an empty/too-small POST (e.g. a few non-UF2 bytes that
      // never form a 512B block), and error==UF2_ERR_OK rejects a malformed/partial UF2.
      // Without this guard a garbage upload returned ok + triggered a pointless reboot.
      bool ended = lt_ota_end(s_otaCtx);
      if (ended && s_otaCtx->error == UF2_ERR_OK && s_otaCtx->bytes_written > 0) {
        s_otaOk = true;
      } else {
        s_otaErr = String("incomplete/invalid image (err ") + (int)s_otaCtx->error +
                   ", wrote " + (unsigned)s_otaCtx->bytes_written + "B)";
      }
    }
    if (s_otaCtx) { free(s_otaCtx); s_otaCtx = nullptr; }
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    if (s_otaCtx) { lt_ota_end(s_otaCtx); free(s_otaCtx); s_otaCtx = nullptr; }
    s_otaErr = "aborted";
  }
}
static void handle_update_done() {
  StaticJsonDocument<128> doc;
  doc["ok"] = s_otaOk;
  if (!s_otaOk) doc["error"] = s_otaErr.length() ? s_otaErr : String("unknown");
  String out; serializeJson(doc, out);
  s_server.send(s_otaOk ? 200 : 400, "application/json", out);
  if (s_otaOk) {
    s_rebootPending = true;
    s_rebootAtMs    = millis() + 750;   // reboot from loop() after the response flushes
  }
}

// Look up an embedded UI asset by request path (exact match).
static const LiteStaticFile *find_static(const String &path) {
  for (unsigned i = 0; i < LITE_STATIC_FILE_COUNT; i++) {
    if (path == LITE_STATIC_FILES[i].path) return &LITE_STATIC_FILES[i];
  }
  return nullptr;
}

// Serve one embedded asset. Binary-safe: sendContent writes raw bytes (gzip and
// PNGs contain NULs). Sockets stream it (bounded by TCP_SND_BUF) — no big mbuf.
static void serve_static(const LiteStaticFile *f) {
  if (f->gzip) s_server.sendHeader("Content-Encoding", "gzip");
  s_server.sendHeader("Cache-Control", "public, max-age=86400");
  s_server.setContentLength(f->len);
  s_server.send(200, f->content_type, "");
  s_server.sendContent((const char *)f->data, (size_t)f->len);
}

// GET / -> the unified web UI's index.html (the app self-provisions in AP mode
// via /scan + /connect, so the same bundle serves STA and AP).
static void handle_root() {
  const LiteStaticFile *f = find_static("/index.html");
  if (f) serve_static(f); else s_server.send(500, "text/plain", "no ui");
}

// Catch-all: serve any embedded asset by path; otherwise a 404. The web UI uses
// HASH routing (#/...), so client routes never hit the server and need no SPA
// fallback. The 404 BODY is JSON, not plain text: the UI's fetch layer
// (httpAPI.js) calls response.json() without checking response.ok, so a
// non-JSON 404 body throws a SyntaxError; {"msg":"error"} parses cleanly and is
// exactly the "endpoint absent / failed" sentinel every store checks for, so
// capability/history probes (e.g. /logs, /limit, /claims/target) degrade right.
static void handle_not_found() {
  const LiteStaticFile *f = find_static(s_server.uri());
  if (f) { serve_static(f); return; }
  s_server.send(404, "application/json", "{\"msg\":\"error\"}");
}

void web_server_lite_begin(LiteEvseManager &mgr, LiteClock &clock, LiteEnergyTotals &totals)
{
  s_mgr_ctrl = &mgr;
  s_clock    = &clock;
  s_totals   = &totals;

  LiteClockConfig cc;
  lite_config_load_clock(cc);
  s_sntpHost    = cc.sntp_hostname;
  s_sntpEnabled = cc.sntp_enabled;
  s_timeZone    = cc.time_zone;
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
  lite_config_load_wizard(s_wizardPassed); // keeps default false if never set (shows wizard)

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
  s_server.on("/update", HTTP_POST, handle_update_done, handle_update_upload);
  s_server.onNotFound(handle_not_found);
  s_server.begin();
  sntp_setoperatingmode(SNTP_OPMODE_POLL);
  sntp_setservername(0, s_sntpHost.c_str());   // s_sntpHost is a stable static String
  if (s_sntpEnabled) sntp_init();              // skipped in manual time mode
}

void web_server_lite_loop()
{
  s_server.handleClient();

  // Keep the clock's UTC offset DST-correct from the configured POSIX TZ. Re-checked
  // ~once a minute (the effective offset only flips at the twice-yearly boundary); both
  // nowLocal() users — the /status local_time and the time-of-day schedule — follow it.
  if (s_clock && s_clock->valid() && s_timeZone.length()) {
    static uint32_t s_tzCheckMs = 0;
    uint32_t nowMs = millis();
    if (s_tzCheckMs == 0 || (nowMs - s_tzCheckMs) >= 60000u) {
      s_tzCheckMs = nowMs;
      int bar = s_timeZone.indexOf('|');
      if (bar >= 0) {
        int eff = lite_tz_offset_min(s_timeZone.c_str() + bar + 1, s_clock->nowUtc(nowMs));
        if (eff != s_clock->tzOffsetMinutes()) s_clock->setTzOffsetMinutes(eff);
      }
    }
  }
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
      s_shaperUpdated = false;
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
      s_shaperCur = cap; s_shaperUpdated = true;   // cache for /status
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
    s_shaperCur = 0; s_shaperUpdated = false;
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
    s_divertChargeRate = r.charge_rate_a;   // cache for /status charge_rate

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
    s_divertChargeRate = 0;
  }

  // MQTT telemetry: per-field publish on cadence (faster while charging). No-op when
  // disabled or WiFi down. Reuses the /status doc builder so fields stay in sync.
  s_mqtt.loop(millis(), s_mgr_ctrl && s_mgr_ctrl->isCharging(),
              &web_server_lite_build_status);
}
#endif
