#pragma once

// JuiceBox charge-current clamp policy. Pure integer functions (no Arduino
// deps) so they unit-test in the native doctest env. Shared by web_server_lite
// (HTTP set path + boot apply). Intentionally NOT guarded by OPENEVSE_LITE: the
// native test env does not define it and must still compile this unit.

static constexpr int JB_MIN_CURRENT = 6;   // J1772 pilot floor
static constexpr int JB_ABS_MAX     = 48;  // absolute safety cap regardless of config

// Clamp a service-max ceiling into [JB_MIN_CURRENT, JB_ABS_MAX].
int lite_clamp_service_max(int hard);

// Clamp a charge-current setpoint into [JB_MIN_CURRENT, lite_clamp_service_max(hard)].
int lite_clamp_charge_current(int soft, int hard);
