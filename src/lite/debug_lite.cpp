#ifdef OPENEVSE_LITE
// ---------------------------------------------------------------------------
// Lite debug.cpp replacement.
//
// debug.h defines RAPI_PORT == SerialEvse and DEBUG_PORT == SerialDebug, both
// StreamSpy wrappers the EVSE core talks through. The full firmware's debug.cpp
// wires them to two separate ESP32 UARTs; on the JuiceBox there is only one
// USART (USART0 LOC1, PE7=TX/PE6=RX), which is the RAPI line to the OpenEVSE
// controller. So:
//   - SerialEvse wraps Serial @ 9600 8N1 (the RAPI line).
//   - SerialDebug also wraps Serial but is never written to this slice
//     (ENABLE_DEBUG is off, so the DBUG* macros are no-ops). Keeping it bound to
//     Serial is harmless and satisfies the extern.
//
// No raw Serial.print* anywhere — that would corrupt RAPI framing. Observability
// is WiFi + curl /status only.
// ---------------------------------------------------------------------------
#include <StreamSpy.h>
#include "debug.h"

StreamSpy SerialDebug(Serial);
StreamSpy SerialEvse(Serial);

void debug_setup()
{
  // Single shared UART: bring up Serial once at the RAPI baud, then attach both
  // StreamSpy wrappers. 8N1 is the LibreTiny Serial default.
  Serial.begin(9600);
  SerialEvse.begin(2048);
  SerialDebug.begin(256);
}

#endif // OPENEVSE_LITE
