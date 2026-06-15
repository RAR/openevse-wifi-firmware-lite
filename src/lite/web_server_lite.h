#pragma once
#ifdef OPENEVSE_LITE
#include <ArduinoJson.h>
class LiteEvseManager;
class LiteClock;
struct LiteEnergyTotals;
void web_server_lite_begin(LiteEvseManager &mgr, LiteClock &clock, LiteEnergyTotals &totals);
void web_server_lite_loop();
// Fill `doc` with the same fields /status serializes (shared by /status + MQTT telemetry).
void web_server_lite_build_status(JsonDocument &doc);
// Boot glue selects which bundle GET / serves and whether to drive STA-retry.
void web_server_lite_set_ap_mode(bool ap);
bool web_server_lite_in_ap_mode(void);
#endif
