#ifdef OPENEVSE_LITE
#include <Arduino.h>
#include <WiFi.h>

#include "em_cmu.h"   // CMU_ClockEnable — GPIO clock for the PF11 reset-line drive
#include "em_gpio.h"  // GPIO_PinModeSet — hold the ATmega RESET deasserted (see setup())

#include "espal_lite.h"
#include "web_server_lite.h"
#include "lite_evse_backend.h"
#include "lite_config_store.h"
#include "lite_evse_manager.h"
#include "lite_clock.h"
#include "lite_energy_totals.h"
#include "lite_led.h"
#include "WiFiStatusLed.h"   // ltWifiStatusLedEnable — WiFi lib header, on the path via <WiFi.h>
#include "manual.h"
#include "lite_provision.h"

#if defined(LITE_EVSE_BACKEND_JUICEBOX)
#include "juicebox_backend.h"
static JuiceBoxBackend s_backend(Serial);   // USART0 LOC1 (PE7=TX/PE6=RX) @ 9600 8N1
#else
#error "No lite EVSE backend selected (define LITE_EVSE_BACKEND_*)"
#endif

// Control seam: the manager owns the apply path; manual is the lifted canary
// claim client (referenced extern from web_server_lite.cpp for /override + status).
static LiteEvseManager s_manager(s_backend);
static LiteClock        s_clock;
static LiteEnergyTotals s_totals;
static bool s_wasCharging = false;  // for the charge->idle accrual edge
ManualOverride manual(s_manager);

// WiFi creds are provisioned at runtime via softAP (D1 stored-only — no
// compile-time creds; see lite_provision). Boot tries stored creds with a bounded
// connect; falls to an open softAP on no-creds-or-connect-fail (D2). A unit that
// fell back because stored creds FAILED periodically retries STA (D3).
#ifndef LITE_STA_CONNECT_TIMEOUT_MS
#define LITE_STA_CONNECT_TIMEOUT_MS 60000u
#endif
#ifndef LITE_AP_RETRY_INTERVAL_MS
#define LITE_AP_RETRY_INTERVAL_MS 300000u
#endif
// Setup-AP passphrase. The WF200 rejects start_ap with INVALID_PARAMETER (0x21)
// when management-frame protection is requested on an OPEN network, so the AP
// must be secured. Matches the standard OpenEVSE firmware's softAP password
// (net_manager.cpp); 8 chars = WPA2-PSK minimum.
#ifndef LITE_AP_PASSWORD
#define LITE_AP_PASSWORD "openevse"
#endif
static bool     s_apCredsFailed = false;  // D3: only retry STA if creds existed-but-failed
static uint32_t s_apSinceMs     = 0;      // millis() when the current AP window began

void setup()
{
  // ATmega EVSE-controller RESET (active-low) is wired to host GPIO PF11
  // (continuity-confirmed on the bench 2026-06-13). We previously never configured
  // PF11, so it floated at power-on (EFM32 GPIOs default to disabled/input) and the
  // line drifting/coupling low INTERMITTENTLY held the Atmel in reset — the "silent /
  // flapping comms" symptom. Drive it push-pull HIGH first thing, before anything
  // else, to hold the controller deasserted (running) for the whole session.
  CMU_ClockEnable(cmuClock_GPIO, true);
  GPIO_PinModeSet(gpioPortF, 11, gpioModePushPull, 1);

  // JuiceBox $-protocol line @ 115200 8N1 — HW-confirmed 2026-06-13 (clean $ES/$MD/$WR
  // frame decode at this rate; the earlier 9600 was a stale RAPI-era assumption). No
  // debug prints here: LibreTiny LT logging is LT_LEVEL_NONE so it can't corrupt framing.
  Serial.begin(115200);
  ESPAL.begin();

  lite_config_begin();              // mount FlashDB KVDB (kvs partition) FIRST — creds live here now

  // OTA self-confirm: mark a freshly-OTA'd TRIAL bank healthy so it sticks (else the
  // bootloader auto-reverts after 3 boots). lt_ota_confirm() is idempotent (libretiny
  // feature/silabs-efm32gg11-ota ed495b5): a no-op that touches no flash unless the running
  // bank is a genuine unconfirmed TRIAL. Called here — early, BEFORE WF200 bring-up — so the
  // one real post-OTA metadata write happens with the radio idle (no WiFi-link blip).
  lt_ota_confirm();

  // Stored-only creds (D1). Try them with a bounded connect; on no-creds or a
  // connect that doesn't land within LITE_STA_CONNECT_TIMEOUT_MS, fall to an open
  // softAP at 192.168.4.1 with SSID OpenEVSE-Lite-<shortid> (D2). The wait loop is
  // bounded by lite_provision_decide()'s timeout — no unbounded blocking.
  LiteWifiConfig creds;
  bool haveCreds = lite_config_load_wifi(creds);
  if (haveCreds) {
    WiFi.begin(creds.ssid.c_str(), creds.pass.c_str());
    uint32_t start = millis();
    while (lite_provision_decide(true, WiFi.status() == WL_CONNECTED,
                                 start, millis(), LITE_STA_CONNECT_TIMEOUT_MS)
           == LiteProvisionAction::StaWait) {
      delay(250);
    }
  }
  if (WiFi.status() == WL_CONNECTED) {
    web_server_lite_set_ap_mode(false);
  } else {
    char ssid[32];
    lite_provision_ap_ssid(ESPAL.getShortId().c_str(), ssid, sizeof(ssid));
    WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
    WiFi.softAP(ssid, LITE_AP_PASSWORD);  // WPA2 — open AP is rejected by the WF200 (see LITE_AP_PASSWORD)
    web_server_lite_set_ap_mode(true);
    s_apCredsFailed = haveCreds;      // D3: retry STA only if creds existed-but-failed
    s_apSinceMs     = millis();
  }

  s_backend.begin();
  if (!lite_config_load_totals(s_totals)) {
    energy_totals_init(s_totals);   // first boot / key absent
  }
  web_server_lite_begin(s_manager, s_clock, s_totals); // loads config -> clamps -> seeds manager target

  // Now that Serial + the backend parser are up, give the Atmel a clean, synchronized
  // restart via its RESET line (PF11, active-low) so we capture its full boot burst —
  // the $HW/$FW/$PV identity and the $WC handshake nonce — from frame zero. (Held high
  // since the top of setup(), the Atmel already booted once before we were listening,
  // so its identity went unseen; this re-announces it with loop() about to run.)
  // Unconditional on every host boot (user-approved 2026-06-13): the WGM160P only
  // reboots on power-cycle/OTA/crash, so a clean comms re-sync is worth interrupting a
  // charge in those rare cases — the Atmel re-establishes its own safe state on reset.
  GPIO_PinOutClear(gpioPortF, 11);  // assert RESET — hold the Atmel
  delay(50);                        // well past the AVR min reset pulse width
  GPIO_PinOutSet(gpioPortF, 11);    // release RESET — Atmel boots fresh
  delay(100);                       // brief settle; loop() catches the boot frames

  // Take ownership of the RGB LED from the WiFi backend and show EVSE state instead of
  // the WiFi bring-up ladder. ltWifiStatusLedEnable(false) makes the WiFi LED calls no-op.
#if defined(LED_R) && defined(LED_G) && defined(LED_B)
  ltWifiStatusLedEnable(false);
  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);
#endif
}

void loop()
{
  // D3 auto-recovery: when AP because stored creds FAILED, periodically drop the
  // AP and re-attempt STA so a unit knocked off by a flaky router rejoins itself.
  // A no-creds unit (s_apCredsFailed=false) stays in AP indefinitely (nothing to retry).
  if (web_server_lite_in_ap_mode() &&
      lite_provision_should_retry_sta(s_apCredsFailed, s_apSinceMs, millis(),
                                      LITE_AP_RETRY_INTERVAL_MS)) {
    WiFi.softAPdisconnect(true);
    LiteWifiConfig c;
    if (lite_config_load_wifi(c)) {
      WiFi.begin(c.ssid.c_str(), c.pass.c_str());
      uint32_t start = millis();
      while (lite_provision_decide(true, WiFi.status() == WL_CONNECTED,
                                   start, millis(), LITE_STA_CONNECT_TIMEOUT_MS)
             == LiteProvisionAction::StaWait) {
        delay(250);
      }
    }
    if (WiFi.status() == WL_CONNECTED) {
      web_server_lite_set_ap_mode(false);
    } else {
      char ssid[32];
      lite_provision_ap_ssid(ESPAL.getShortId().c_str(), ssid, sizeof(ssid));
      WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
      WiFi.softAP(ssid, LITE_AP_PASSWORD);
      s_apSinceMs = millis();           // reset the retry window
    }
  }

  web_server_lite_loop();
  s_backend.loop();
  s_manager.loop();   // ticks session energy; fires its own session-complete edge

  // Mirror the manager's charge->idle edge to bank the finished session's Wh into the
  // persistent lifetime totals. Session energy freezes on stop (resets only on the next
  // rising edge), so getSessionWattHours() still holds the completed total here.
  bool charging = s_manager.isCharging();
  if (s_wasCharging && !charging) {
    energy_totals_add(s_totals, s_manager.getSessionWattHours(),
                      s_clock.nowLocal(millis()), s_clock.valid());
    lite_config_save_totals(s_totals);
  }
  s_wasCharging = charging;

  // EVSE-state RGB indicator (active-high). Pure mapping in lite_led; this is the only
  // device-side glue. Compiled out on boards without the LED_R/G/B variant macros.
#if defined(LED_R) && defined(LED_G) && defined(LED_B)
  {
    LiteLedSpec spec = lite_led_for(s_manager.getDeviceState(),
                                    s_manager.getState() == EvseState::Disabled,
                                    s_backend.isOnline());
    bool on = lite_led_phase_on(spec.pattern, millis());
    digitalWrite(LED_R, (spec.color.r && on) ? HIGH : LOW);
    digitalWrite(LED_G, (spec.color.g && on) ? HIGH : LOW);
    digitalWrite(LED_B, (spec.color.b && on) ? HIGH : LOW);
  }
#endif
}
#endif
