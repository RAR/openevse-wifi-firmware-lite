#include "lite_input_filter.h"
#include <math.h>

#ifndef LITE_FILTER_MIN_TAU
#define LITE_FILTER_MIN_TAU 10u   // minimum tau (s), matches upstream INPUT_FILTER_MIN_TAU
#endif

double lite_input_filter(double input, double filtered, uint32_t tau_s, uint32_t delta_s)
{
  double factor;
  if (tau_s > 0) {
    if (tau_s < LITE_FILTER_MIN_TAU) tau_s = LITE_FILTER_MIN_TAU;
    factor = 1.0 - exp(-1.0 * ((double)delta_s / (double)tau_s));
  } else {
    factor = 1.0;   // tau 0 => no filtering
  }
  return filtered + factor * (input - filtered);
}
