#pragma once
#include <stddef.h>
#include <stdint.h>
typedef void (*lite_random_backend_t)(uint8_t *buf, size_t len);
void lite_random_set_backend(lite_random_backend_t fn);  // test/integration injection
// Fills buf with len random bytes via the registered backend.
// Contract: a null buf or len==0 is a safe no-op. If NO backend has been
// registered, buf is ZERO-FILLED (not random) — callers requiring entropy must
// ensure a backend is set at boot and must treat all-zero output as a
// programming error, never as acceptable entropy.
void lite_random_bytes(uint8_t *buf, size_t len);
