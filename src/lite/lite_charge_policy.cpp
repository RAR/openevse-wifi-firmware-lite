#include "lite_charge_policy.h"

static int clamp_int(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

int lite_clamp_service_max(int hard) {
  return clamp_int(hard, JB_MIN_CURRENT, JB_ABS_MAX);
}

int lite_clamp_charge_current(int soft, int hard) {
  return clamp_int(soft, JB_MIN_CURRENT, lite_clamp_service_max(hard));
}
