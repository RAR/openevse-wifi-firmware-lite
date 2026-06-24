#pragma once
#ifdef OPENEVSE_LITE
#include <ArduinoJson.h>
#include "lite_evse_state.h"
#include "lite_evse_claims.h"

// Backend-agnostic EVSE device seam. web_server_lite + main_lite depend ONLY on this.
class LiteEvseBackend {
public:
  virtual ~LiteEvseBackend() {}
  virtual void begin() = 0;
  virtual void loop()  = 0;

  virtual bool          isOnline() const = 0;
  virtual LiteEvseState getState() const = 0;
  virtual int           getAmps()  const = 0;
  virtual int           getPower() const = 0;
  virtual int           getTemp()  const = 0;
  virtual int           getFault() const = 0;

  // Control seam (write surface). Slice 1 ships only the single charge-current
  // setpoint; the full claim/priority model arrives in Slice 1.5.
  virtual void setChargeCurrent(int amps) = 0; // desired charge current (A); backend may clamp to its own floor
  virtual int  getChargeCurrent() const = 0;   // current advertised setpoint

  // Control-state axis (Slice 1.5). Active => drive the setpoint; Disabled => stop.
  virtual void setState(EvseState state) = 0;
  virtual bool isCharging() const = 0;             // device is actively charging
  virtual int  getMinCurrent() const = 0;          // J1772 floor
  virtual int  getMaxHardwareCurrent() const = 0;  // install/service-max rating
  virtual void setMaxHardwareCurrent(int a) = 0;   // manager seeds this from config
  virtual int  getTemperature() const = 0;         // $ES T (single sensor)
  virtual bool isTemperatureValid() const = 0;
  virtual int  getEvseState() const = 0;           // raw $ES S code

  // EVSE-controller (MCU) identity, for the "OpenEVSE firmware" UI field. Empty when the
  // backend has no separate controller (default); JuiceBox fills these from the ATmega's
  // $FW/$HW/$PV handshake replies. Returned pointers must stay valid (member buffers).
  virtual const char *getControllerFirmware() const { return ""; }
  virtual const char *getControllerHardware() const { return ""; }
  virtual const char *getControllerProtocol() const { return ""; }

  // Backend-specific extras (identity strings, raw fields, ...).
  virtual void addStatusFields(JsonDocument &doc) const = 0;
};
#endif // OPENEVSE_LITE
