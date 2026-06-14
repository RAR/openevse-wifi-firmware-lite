#pragma once
#ifdef OPENEVSE_LITE
#include <Arduino.h>
#include <string>

class EspalLite {
public:
  void begin();
  uint32_t getFreeHeap();
  String getShortId();   // 6 hex of EFM32 DEVINFO unique id
  String getLongId();    // 16 hex of EFM32 DEVINFO unique id
  uint32_t getFlashChipSize();
  String getChipInfo();
  void reset();          // NVIC_SystemReset via LibreTiny
  void eraseConfig();    // clear the LittleFS config region (Task 4 owns the store)
};

extern EspalLite ESPAL;
#endif
