#include "lite_provision.h"
#include <string.h>
#include <stdio.h>

LiteProvisionAction lite_provision_decide(bool has_creds, bool sta_connected,
                                          uint32_t attempt_start_ms, uint32_t now_ms,
                                          uint32_t timeout_ms) {
  if (sta_connected) return LiteProvisionAction::StaOnline;
  if (!has_creds)    return LiteProvisionAction::EnterAp;
  uint32_t elapsed = (uint32_t)(now_ms - attempt_start_ms);
  return (elapsed >= timeout_ms) ? LiteProvisionAction::EnterAp
                                 : LiteProvisionAction::StaWait;
}

bool lite_provision_should_retry_sta(bool creds_failed, uint32_t ap_since_ms,
                                     uint32_t now_ms, uint32_t interval_ms) {
  if (!creds_failed) return false;
  return (uint32_t)(now_ms - ap_since_ms) >= interval_ms;
}

void lite_provision_ap_ssid(const char *shortid, char *out, size_t cap) {
  if (!out || cap == 0) return;
  snprintf(out, cap, "OpenEVSE-Lite-%s", shortid ? shortid : "");
}

int lite_provision_enc(int auth_mode) { return auth_mode == 0 ? 0 : 1; }

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

size_t lite_url_decode(const char *in, size_t in_len, char *out, size_t cap) {
  size_t o = 0;
  for (size_t i = 0; i < in_len && o + 1 < cap; i++) {
    char c = in[i];
    if (c == '+') { out[o++] = ' '; }
    else if (c == '%' && i + 2 < in_len) {
      int hi = hexval(in[i+1]), lo = hexval(in[i+2]);
      if (hi >= 0 && lo >= 0) { out[o++] = (char)((hi << 4) | lo); i += 2; }
      else { out[o++] = c; }            // malformed -> literal '%'
    } else { out[o++] = c; }            // includes truncated trailing '%'
  }
  if (cap > 0) out[o] = '\0';
  return o;
}
