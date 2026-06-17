#ifdef OPENEVSE_LITE
#include "lite_config_store.h"
#include "lite_energy_totals.h"

#include <flashdb.h>
#include <string.h>

// FlashDB KVDB config store on the `kvs` FAL partition (FLASH_KVS_OFFSET
// 0x1F0000 + 0x8000). FlashDB is framework-vendored (libflashdb.a); the EFM32
// build sets FDB_WRITE_GRAN=32 (word-only flash) so KVs survive reboot — proven
// on the bench. LibreTiny ships only the IPreferences interface (no concrete
// Preferences class), so FlashDB is the real KV API here.

static struct fdb_kvdb s_kvdb;
static bool            s_ready = false;

bool lite_config_begin()
{
  // "env" = db name, "kvs" = FAL partition name, NULL lock (single-threaded loop()).
  fdb_err_t err = fdb_kvdb_init(&s_kvdb, "env", "kvs", NULL, NULL);
  s_ready = (err == FDB_NO_ERR);
  return s_ready;
}

// --- string helpers (WiFi creds) ---

static bool kv_get_str(const char *key, String &out)
{
  if (!s_ready) return false;
  char buf[64] = {0};
  struct fdb_blob blob;
  fdb_kv_get_blob(&s_kvdb, key, fdb_blob_make(&blob, buf, sizeof(buf) - 1));
  if (blob.saved.len == 0) return false; // key absent
  buf[sizeof(buf) - 1] = '\0';
  out = buf;
  return true;
}

static bool kv_set_str(const char *key, const String &val)
{
  if (!s_ready) return false;
  struct fdb_blob blob;
  // +1 to persist the trailing NUL so reads are always terminated.
  fdb_err_t err = fdb_kv_set_blob(
      &s_kvdb, key, fdb_blob_make(&blob, val.c_str(), val.length() + 1));
  return err == FDB_NO_ERR;
}

// --- int helpers (EVSE config) ---

static bool kv_get_int(const char *key, int &out)
{
  if (!s_ready) return false;
  int v = 0;
  struct fdb_blob blob;
  fdb_kv_get_blob(&s_kvdb, key, fdb_blob_make(&blob, &v, sizeof(v)));
  if (blob.saved.len == 0) return false; // key absent
  out = v;
  return true;
}

static bool kv_set_int(const char *key, int v)
{
  if (!s_ready) return false;
  struct fdb_blob blob;
  fdb_err_t err = fdb_kv_set_blob(&s_kvdb, key, fdb_blob_make(&blob, &v, sizeof(v)));
  return err == FDB_NO_ERR;
}

// --- WiFi creds ---

bool lite_config_load_wifi(LiteWifiConfig &out)
{
  String ssid;
  if (!kv_get_str("wifi_ssid", ssid) || ssid.length() == 0) {
    return false; // no/blank ssid -> fall back to build-flag defaults
  }
  out.ssid = ssid;
  String pass;
  out.pass = kv_get_str("wifi_pass", pass) ? pass : String("");
  return true;
}

bool lite_config_save_wifi(const LiteWifiConfig &in)
{
  return kv_set_str("wifi_ssid", in.ssid) && kv_set_str("wifi_pass", in.pass);
}

// --- EVSE config ---

bool lite_config_load_evse(LiteEvseConfig &out)
{
  int hard = 0;
  if (!kv_get_int("max_current_hard", hard)) {
    return false; // unset store -> caller applies defaults
  }
  out.max_current_hard = hard;
  int soft = hard;
  out.max_current_soft = kv_get_int("max_current_soft", soft) ? soft : hard;
  return true;
}

bool lite_config_save_evse(const LiteEvseConfig &in)
{
  return kv_set_int("max_current_hard", in.max_current_hard) &&
         kv_set_int("max_current_soft", in.max_current_soft);
}

bool lite_config_load_wizard(bool &out)
{
  int v = 0;
  if (!kv_get_int("wizard_passed", v)) return false;
  out = (v != 0);
  return true;
}

bool lite_config_save_wizard(bool passed)
{
  return kv_set_int("wizard_passed", passed ? 1 : 0);
}

void lite_config_erase()
{
  if (!s_ready) return;
  fdb_kv_del(&s_kvdb, "wifi_ssid");
  fdb_kv_del(&s_kvdb, "wifi_pass");
}

// --- Energy totals (POD blob) ---

bool lite_config_load_totals(LiteEnergyTotals &out)
{
  if (!s_ready) return false;
  struct fdb_blob blob;
  fdb_kv_get_blob(&s_kvdb, "energy_totals", fdb_blob_make(&blob, &out, sizeof(out)));
  return blob.saved.len == sizeof(out);   // full struct present
}

bool lite_config_save_totals(const LiteEnergyTotals &in)
{
  if (!s_ready) return false;
  struct fdb_blob blob;
  return fdb_kv_set_blob(&s_kvdb, "energy_totals",
                         fdb_blob_make(&blob, &in, sizeof(in))) == FDB_NO_ERR;
}

// --- Weekly schedule (POD blob) ---

bool lite_config_load_schedule(LiteSchedule &out)
{
  if (!s_ready) return false;
  struct fdb_blob blob;
  fdb_kv_get_blob(&s_kvdb, "sched", fdb_blob_make(&blob, &out, sizeof(out)));
  return blob.saved.len == sizeof(out);   // full struct present
}

bool lite_config_save_schedule(const LiteSchedule &in)
{
  if (!s_ready) return false;
  struct fdb_blob blob;
  return fdb_kv_set_blob(&s_kvdb, "sched",
                         fdb_blob_make(&blob, &in, sizeof(in))) == FDB_NO_ERR;
}

// --- Solar-divert config (POD blob) ---

bool lite_config_load_divert(LiteDivertConfig &out)
{
  if (!s_ready) return false;
  struct fdb_blob blob;
  fdb_kv_get_blob(&s_kvdb, "divert", fdb_blob_make(&blob, &out, sizeof(out)));
  return blob.saved.len == sizeof(out);
}

bool lite_config_save_divert(const LiteDivertConfig &in)
{
  if (!s_ready) return false;
  struct fdb_blob blob;
  return fdb_kv_set_blob(&s_kvdb, "divert",
                         fdb_blob_make(&blob, &in, sizeof(in))) == FDB_NO_ERR;
}

// --- Load-shaper config (POD blob) ---

bool lite_config_load_shaper(LiteShaperConfig &out)
{
  if (!s_ready) return false;
  struct fdb_blob blob;
  fdb_kv_get_blob(&s_kvdb, "shaper", fdb_blob_make(&blob, &out, sizeof(out)));
  return blob.saved.len == sizeof(out);
}
bool lite_config_save_shaper(const LiteShaperConfig &in)
{
  if (!s_ready) return false;
  struct fdb_blob blob;
  return fdb_kv_set_blob(&s_kvdb, "shaper",
                         fdb_blob_make(&blob, &in, sizeof(in))) == FDB_NO_ERR;
}

// --- Clock config (sntp_hostname + tz_offset_min) ---

bool lite_config_load_clock(LiteClockConfig &out)
{
  out.sntp_hostname = "pool.ntp.org";   // defaults
  out.tz_offset_min = 0;
  out.time_zone     = "";
  out.sntp_enabled  = true;             // NTP on by default
  if (!s_ready) return false;
  kv_get_str("sntp_hostname", out.sntp_hostname);
  kv_get_int("tz_offset_min", out.tz_offset_min);
  kv_get_str("time_zone", out.time_zone);
  int en = 1; if (kv_get_int("sntp_enabled", en)) out.sntp_enabled = (en != 0);
  return true;
}

bool lite_config_save_clock(const LiteClockConfig &in)
{
  if (!s_ready) return false;
  bool ok = kv_set_str("sntp_hostname", in.sntp_hostname);
  ok = kv_set_int("tz_offset_min", in.tz_offset_min) && ok;
  ok = kv_set_str("time_zone", in.time_zone) && ok;
  ok = kv_set_int("sntp_enabled", in.sntp_enabled ? 1 : 0) && ok;
  return ok;
}

// --- MQTT telemetry config (per-key; String members preclude a blob) ---

bool lite_config_load_mqtt(LiteMqttConfig &out)
{
  out.enabled  = false;          // defaults
  out.server   = "";
  out.port     = 1883;
  out.topic    = "";
  out.user     = "";
  out.pass     = "";
  out.period_s = 30;
  if (!s_ready) return false;
  int en = 0, port = 1883, period = 30;
  bool any = false;
  if (kv_get_int("mqtt_enabled", en)) { out.enabled = (en != 0); any = true; }
  { String s; if (kv_get_str("mqtt_server", s)) { out.server = s; any = true; } }
  if (kv_get_int("mqtt_port", port))   { out.port = port; any = true; }
  { String s; if (kv_get_str("mqtt_topic", s)) { out.topic = s; any = true; } }
  { String s; if (kv_get_str("mqtt_user",  s)) { out.user = s;  any = true; } }
  { String s; if (kv_get_str("mqtt_pass",  s)) { out.pass = s;  any = true; } }
  if (kv_get_int("mqtt_period", period)) { out.period_s = (uint32_t)period; any = true; }
  return any;
}

bool lite_config_save_mqtt(const LiteMqttConfig &in)
{
  if (!s_ready) return false;
  bool ok = kv_set_int("mqtt_enabled", in.enabled ? 1 : 0);
  ok = kv_set_str("mqtt_server", in.server)            && ok;
  ok = kv_set_int("mqtt_port",   in.port)              && ok;
  ok = kv_set_str("mqtt_topic",  in.topic)             && ok;
  ok = kv_set_str("mqtt_user",   in.user)              && ok;
  ok = kv_set_str("mqtt_pass",   in.pass)              && ok;
  ok = kv_set_int("mqtt_period", (int)in.period_s)     && ok;
  return ok;
}
#endif
