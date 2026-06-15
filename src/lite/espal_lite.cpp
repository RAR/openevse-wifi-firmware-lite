#ifdef OPENEVSE_LITE
#include "espal_lite.h"
#include "espal_lite_format.h"

#include <libretiny.h>
#include "lt_family.h"   // pulls in em_device.h -> DEVINFO + RMU register defs (Gecko SDK)

// Reset cause latched in begin() before anything can clear it. RMU->RSTCAUSE bits are
// sticky and accumulate across resets (and PORST has top priority), so we read once at
// boot then clear, leaving the NEXT reset's cause unambiguous. See getRebootReason().
// (emlib RMU_ResetCauseClear() isn't compiled into the linked emlib subset, but on
// GG11 — Series 1, no EMU_AUXCTRL_HRCCLR — it reduces to just `RMU->CMD = RMU_CMD_RCCLR`,
// which clears every cause bit incl. PORST. So we issue that register write directly.)
static lt_reboot_reason_t s_rebootReason = REBOOT_REASON_UNKNOWN;

// EFM32 DEVINFO unique id source: DEVINFO->UNIQUEL (bits 31:0) and DEVINFO->UNIQUEH
// (bits 63:32). LibreTiny's lt_cpu_get_unique_id() returns only the low 24 bits, so
// read DEVINFO directly to recover the full 64-bit id.
static inline uint64_t lite_efm32_uid64() {
  return ((uint64_t)DEVINFO->UNIQUEH << 32) | (uint64_t)DEVINFO->UNIQUEL;
}

void EspalLite::begin() {
  // Latch the reset cause first thing, then clear RMU->RSTCAUSE so the NEXT reset
  // reports a clean, unambiguous cause. ESPAL.begin() is the earliest app hook
  // (called before WiFi/FlashDB in setup), so nothing has perturbed RSTCAUSE yet.
  s_rebootReason = lt_get_reboot_reason();
  RMU->CMD = RMU_CMD_RCCLR;   // clear RSTCAUSE so the next reset's cause is unambiguous
}

String EspalLite::getRebootReason() {
  switch (s_rebootReason) {
    case REBOOT_REASON_POWER:      return F("power");      // POR
    case REBOOT_REASON_BROWNOUT:   return F("brownout");   // AVDD BOD
    case REBOOT_REASON_HARDWARE:   return F("hardware");   // external reset pin
    case REBOOT_REASON_SOFTWARE:   return F("software");   // NVIC_SystemReset (e.g. post-/connect)
    case REBOOT_REASON_WATCHDOG:   return F("watchdog");
    case REBOOT_REASON_CRASH:      return F("crash");      // lockup (hard fault)
    case REBOOT_REASON_SLEEP_GPIO: return F("sleep_gpio");
    case REBOOT_REASON_SLEEP_RTC:  return F("sleep_rtc");
    case REBOOT_REASON_SLEEP_USB:  return F("sleep_usb");
    case REBOOT_REASON_DEBUGGER:   return F("debugger");
    default:                       return F("unknown");
  }
}

uint32_t EspalLite::getFreeHeap() {
  return (uint32_t)xPortGetFreeHeapSize();
}

String EspalLite::getShortId() {
  uint64_t uid = lite_efm32_uid64();
  return String(lite_format_short_id(uid).c_str());
}

String EspalLite::getLongId() {
  uint64_t uid = lite_efm32_uid64();
  return String(lite_format_long_id(uid).c_str());
}

uint32_t EspalLite::getFlashChipSize() {
  // WGM160P has 2 MB internal flash.
  return lite_flash_size_bytes(0x200000);
}

String EspalLite::getChipInfo() {
  return String("EFM32GG11B820/WF200");
}

void EspalLite::reset() {
  NVIC_SystemReset();
}

void EspalLite::eraseConfig() {
  // Factory-reset hook: no-op stub for now. Config (incl. stored-only WiFi creds)
  // lives in the FlashDB KVS — see lite_config_store; a real wipe would clear that
  // KVDB rather than any filesystem.
}

EspalLite ESPAL;
#endif
