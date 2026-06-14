#pragma once
#ifdef OPENEVSE_LITE
#include "evse_man.h"   // resolves to the lite compat shim

class ManualOverride
{
  private:
    EvseManager *_evse;
    uint8_t _version;
  public:
    ManualOverride(EvseManager &evse);
    ~ManualOverride();

    bool claim();
    bool claim(EvseProperties &props);
    bool release();
    bool toggle();

    bool isActive() {
      return _evse->clientHasClaim(EvseClient_OpenEVSE_Manual);
    }
    bool getProperties(EvseProperties &props);
    uint8_t getVersion();
    uint8_t setVersion(uint8_t version);
};
#endif
