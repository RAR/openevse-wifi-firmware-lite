#pragma once
#include <stdint.h>
#include <stddef.h>

// Pure WiFi provisioning decision logic (no radio, no I/O) — native-testable.

enum class LiteProvisionAction : uint8_t { StaWait = 0, StaOnline = 1, EnterAp = 2 };

// Boot/runtime decision. elapsed = (uint32_t)(now_ms - attempt_start_ms) (wrap-safe).
//  connected            -> StaOnline
//  !has_creds           -> EnterAp
//  elapsed >= timeout    -> EnterAp
//  else                 -> StaWait
LiteProvisionAction lite_provision_decide(bool has_creds, bool sta_connected,
                                          uint32_t attempt_start_ms, uint32_t now_ms,
                                          uint32_t timeout_ms);

// True when an AP-mode unit (that fell back because stored creds failed) should
// re-attempt STA. since = (uint32_t)(now_ms - ap_since_ms). Never true if !creds_failed.
bool lite_provision_should_retry_sta(bool creds_failed, uint32_t ap_since_ms,
                                     uint32_t now_ms, uint32_t interval_ms);

// Writes "OpenEVSE-Lite-<shortid>" into out, NUL-terminated, truncated to cap.
void lite_provision_ap_ssid(const char *shortid, char *out, size_t cap);

// Maps a platform auth-mode enum to the /scan enc field: 0 (open) iff auth_mode==0.
int lite_provision_enc(int auth_mode);

// URL-decodes in[0..in_len) into out (cap incl. NUL). '+' -> space, %HH -> byte,
// malformed/truncated '%' sequences pass through literally. Returns decoded length.
size_t lite_url_decode(const char *in, size_t in_len, char *out, size_t cap);
