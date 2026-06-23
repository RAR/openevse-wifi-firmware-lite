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
#ifdef LITE_RFID_SCAN
#include "lite_rfid_scan.h"
#endif

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

#if defined(LITE_LED_SELFTEST) && defined(LED_R) && defined(LED_G) && defined(LED_B)
// Gated bench validation of the LibreTiny analogWrite() PWM primitive (NOT in release
// builds). Runs once at the end of setup(), then loop() drives the normal indicator.
// Sequence mirrors the libretiny PWM bench checklist so the result maps 1:1 to it:
//   1 routing    — ramp R, then G, then B alone 0->255 (others off): each must ramp
//                  smoothly and be the correct physical color (wrong color = bad LOC/timer).
//   2 run-update — on red, 64 then 200 via analogWrite: the buffered-update path (no host
//                  test); brightness must step cleanly, no glitch.
//   3 handoff    — digitalWrite(LED_R, LOW) after analogWrite must fully extinguish, then
//                  analogWrite re-arms it (proves PWM route releases + re-inits).
//   5 color mix  — true orange (255/70/0), white, purple held for eyeballing the hue.
// (4 no-flicker is observable throughout.) analogWriteFrequency(300) is set by the caller.
static void lite_led_selftest()
{
  const int led[3] = { LED_R, LED_G, LED_B };
  // Repeat the whole sweep a few times so the bench has a comfortable window to watch and
  // re-confirm each phase (it runs once per boot; this avoids a "missed it" reflash).
  for (int rep = 0; rep < 3; rep++) {
    // 1. Per-channel routing ramp.
    for (int ch = 0; ch < 3; ch++) {
      analogWrite(led[0], 0); analogWrite(led[1], 0); analogWrite(led[2], 0);
      for (int v = 0; v <= 255; v += 5) { analogWrite(led[ch], v); delay(15); }
      delay(500);
      analogWrite(led[ch], 0);
      delay(300);
    }
    // 2. Running buffered update on red.
    analogWrite(LED_R, 64);  delay(900);
    analogWrite(LED_R, 200); delay(900);
    // 3. GPIO handoff then PWM re-arm.
    digitalWrite(LED_R, LOW); delay(900);
    analogWrite(LED_R, 255);  delay(500);
    analogWrite(LED_R, 0);    delay(300);
    // 5. Color mixes — hold each for a clear eyeball read.
    analogWrite(LED_R, 255); analogWrite(LED_G, 70);  analogWrite(LED_B, 0);   delay(1800); // orange
    analogWrite(LED_R, 255); analogWrite(LED_G, 255); analogWrite(LED_B, 255); delay(1800); // white
    analogWrite(LED_R, 255); analogWrite(LED_G, 0);   analogWrite(LED_B, 255); delay(1800); // purple
    analogWrite(LED_R, 0); analogWrite(LED_G, 0); analogWrite(LED_B, 0);
    delay(600);
  }
}
#endif

#if defined(LED_R) && defined(LED_G) && defined(LED_B)
// Drive the "powering on" orange breathe (the commsOnline=false indicator) at nowMs.
// Called from setup() — before loop() runs — so the LED breathes from the first moment
// instead of sitting dark until the app takes the LED at the end of setup(). Uses the same
// lite_led_for/envelope path as loop(), so the handoff into loop() is seamless.
// analogWriteFrequency(300) must already be set (see setup()).
static void lite_led_boot_breathe(uint32_t nowMs)
{
  LiteLedSpec spec = lite_led_for(LiteEvseState::Unknown, false, /*commsOnline=*/false, false, false);
  uint8_t env = lite_led_envelope(spec.pattern, nowMs);
  analogWrite(LED_R, (spec.color.r * env) / 255);
  analogWrite(LED_G, (spec.color.g * env) / 255);
  analogWrite(LED_B, (spec.color.b * env) / 255);
}
#endif

void setup()
{
#ifdef LITE_RFID_SCAN
  // Bench bring-up: sweep candidate GPIOs for the CLRC663 (VERSION==0x18 oracle),
  // publish the winning pinout in g_rfid_scan_result for SWD read-back, then carry
  // on with a normal boot so loop() keeps the watchdog fed and RAM intact.
  lite_rfid_scan_run();
#endif

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

  // Light the RGB LED immediately (orange "powering on" breathe) so there's no dark gap
  // while WiFi connects below. The WiFi-LED ladder is already off (LT_WIFI_STATUS_LED_DEFAULT=0);
  // this hands the pins to our PWM driver before the (possibly multi-second) connect, and the
  // connect-wait loop ticks the breathe so it animates. analogWriteFrequency() is a single
  // GLOBAL period a channel latches only when written — set 300 Hz before the first write.
  // Never pinMode/digitalWrite these pins (detaches the PWM route).
#if defined(LED_R) && defined(LED_G) && defined(LED_B)
  ltWifiStatusLedEnable(false);
  analogWriteFrequency(300);
  lite_led_boot_breathe(millis());
#endif

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
#if defined(LED_R) && defined(LED_G) && defined(LED_B)
      lite_led_boot_breathe(millis());   // animate the orange breathe while connecting
#endif
      delay(20);
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

  // The RGB LED was already handed to our PWM driver at the top of setup() (orange boot
  // breathe held through WiFi connect); loop() drives the state indicator from here.

#if defined(LITE_LED_SELFTEST) && defined(LED_R) && defined(LED_G) && defined(LED_B)
  lite_led_selftest();   // gated PWM bench sweep; loop() then drives the normal indicator
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

  // OEM-style RGB indicator (active-high PWM). Pure mapping in lite_led; this is the only
  // device-side glue. Compiled out on boards without the LED_R/G/B variant macros.
#if defined(LED_R) && defined(LED_G) && defined(LED_B)
  {
    LiteLedSpec spec = lite_led_for(s_manager.getDeviceState(),
                                    s_manager.getState() == EvseState::Disabled,
                                    s_backend.isOnline(),
                                    web_server_lite_in_ap_mode(),
                                    WiFi.status() == WL_CONNECTED);
    uint8_t env = lite_led_envelope(spec.pattern, millis());
    // Drive every channel through analogWrite (incl. full 0/255) — mixing a digitalWrite
    // here would detach the PWM route (see setup()). Duty = color * envelope, both 0..255.
    analogWrite(LED_R, (spec.color.r * env) / 255);
    analogWrite(LED_G, (spec.color.g * env) / 255);
    analogWrite(LED_B, (spec.color.b * env) / 255);
  }
#endif
}
#endif
