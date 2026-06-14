#ifdef OPENEVSE_LITE
#include "espal_lite.h"
#include "espal_lite_format.h"

#include <libretiny.h>
#include "lt_family.h"   // pulls in em_device.h -> DEVINFO struct (Gecko SDK)

// EFM32 DEVINFO unique id source: DEVINFO->UNIQUEL (bits 31:0) and DEVINFO->UNIQUEH
// (bits 63:32). LibreTiny's lt_cpu_get_unique_id() returns only the low 24 bits, so
// read DEVINFO directly to recover the full 64-bit id.
static inline uint64_t lite_efm32_uid64() {
  return ((uint64_t)DEVINFO->UNIQUEH << 32) | (uint64_t)DEVINFO->UNIQUEL;
}

void EspalLite::begin() {
  // Nothing to initialise on the EFM32 side for ESPAL itself; peripherals
  // are brought up by their own subsystems.
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
  // Persistence is deferred this slice (the LibreTiny fork ships no LittleFS),
  // so config-erase is a no-op stub. WiFi creds come from build flags.
}

EspalLite ESPAL;
#endif
