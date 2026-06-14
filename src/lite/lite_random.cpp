#include "lite_random.h"
static lite_random_backend_t s_backend = nullptr;
void lite_random_set_backend(lite_random_backend_t fn) { s_backend = fn; }
void lite_random_bytes(uint8_t *buf, size_t len) {
  if (len == 0 || buf == nullptr) return;
  if (s_backend) { s_backend(buf, len); return; }
  for (size_t i = 0; i < len; i++) buf[i] = 0;  // safe default until backend wired
}
