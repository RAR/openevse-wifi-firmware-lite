#pragma once
#include <stdint.h>
#include <string>

inline std::string lite_hex_impl(uint64_t v, int width) {
  static const char *d = "0123456789abcdef";
  std::string s(width, '0');
  for (int i = width - 1; i >= 0; --i) { s[i] = d[v & 0xF]; v >>= 4; }
  return s;
}

inline std::string lite_format_short_id(uint64_t uid) { return lite_hex_impl(uid & 0xFFFFFF, 6); }
inline std::string lite_format_long_id(uint64_t uid)  { return lite_hex_impl(uid, 16); }
inline uint32_t    lite_flash_size_bytes(uint32_t reported) { return reported; }
