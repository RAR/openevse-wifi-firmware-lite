#pragma once
#include "lite_evse_state.h"

// Map the canonical EVSE state onto the OpenEVSE local-API contract that the
// firstof9/openevse Home Assistant integration consumes (state int + status
// string). `controlDisabled` is the manager's control axis (manual Disabled
// claim): when set it reports OpenEVSE "sleeping" (254) UNLESS the device is
// faulted, in which case the fault wins.
//
// Error -> 8 is best-effort: the JuiceBox $-protocol only tells us fault != 0,
// not the OpenEVSE fault taxonomy. The raw $WR fault string is surfaced
// separately via the `wr` status field.
int         openevse_state_code(LiteEvseState s, bool controlDisabled);
const char *openevse_status_str(LiteEvseState s, bool controlDisabled);
