#pragma once
#include <stdint.h>

// Exponential smoothing toward `input`, transcribed from standard-fw InputFilter:
//   factor = (tau_s>0) ? 1 - exp(-delta_s / max(tau_s, LITE_FILTER_MIN_TAU)) : 1.0
//   result = filtered + factor * (input - filtered)
// tau_s/delta_s in seconds. tau_s==0 disables filtering (returns input). Pure — no millis().
double lite_input_filter(double input, double filtered, uint32_t tau_s, uint32_t delta_s);
