#pragma once
#ifdef OPENEVSE_LITE
class LiteEvseManager;
class LiteClock;
struct LiteEnergyTotals;
void web_server_lite_begin(LiteEvseManager &mgr, LiteClock &clock, LiteEnergyTotals &totals);
void web_server_lite_loop();
// Boot glue selects which bundle GET / serves and whether to drive STA-retry.
void web_server_lite_set_ap_mode(bool ap);
bool web_server_lite_in_ap_mode(void);
#endif
