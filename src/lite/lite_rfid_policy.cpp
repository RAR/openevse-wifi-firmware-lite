#include "lite_rfid_policy.h"
#include <string.h>
#include <ctype.h>

LiteRfidStatus g_lite_rfid_status = {};

void lite_rfid_status_reset(LiteRfidStatus *s)
{
  if (!s) return;
  memset(s, 0, sizeof(*s));
}

static bool is_hex_digit(char c)
{
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

static bool is_sep(char c)
{
  return c == ',' || c == ';' || c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// Validate + upper-case one token into dst (capacity LITE_RFID_UID_HEX_MAX).
// Returns true if the token is a well-formed UID (even length, all hex, fits).
static bool normalize_token(const char *tok, size_t len, char *dst)
{
  if (len == 0 || (len % 2) != 0 || len > (LITE_RFID_UID_HEX_MAX - 1)) return false;
  for (size_t i = 0; i < len; ++i) {
    if (!is_hex_digit(tok[i])) return false;
    dst[i] = (char)toupper((unsigned char)tok[i]);
  }
  dst[len] = '\0';
  return true;
}

uint8_t lite_rfid_allowlist_parse(const char *text, LiteRfidAllowlist *out)
{
  if (!out) return 0;
  out->count = 0;
  if (!text) return 0;

  const char *p = text;
  while (*p && out->count < LITE_RFID_MAX_ALLOW) {
    while (*p && is_sep(*p)) ++p;             // skip separators
    const char *start = p;
    while (*p && !is_sep(*p)) ++p;            // span one token
    size_t len = (size_t)(p - start);
    if (len == 0) continue;
    char norm[LITE_RFID_UID_HEX_MAX];
    if (normalize_token(start, len, norm)) {
      strcpy(out->list[out->count], norm);
      out->count++;
    }
    // malformed tokens are silently skipped
  }
  return out->count;
}

bool lite_rfid_uid_allowed(const char *uid_hex, const LiteRfidAllowlist *allow)
{
  if (!uid_hex || !uid_hex[0] || !allow || allow->count == 0) return false;
  // Normalize the candidate to uppercase for a case-insensitive compare.
  char norm[LITE_RFID_UID_HEX_MAX];
  size_t n = strlen(uid_hex);
  if (n >= LITE_RFID_UID_HEX_MAX) return false;
  for (size_t i = 0; i < n; ++i) norm[i] = (char)toupper((unsigned char)uid_hex[i]);
  norm[n] = '\0';
  for (uint8_t i = 0; i < allow->count; ++i)
    if (strcmp(norm, allow->list[i]) == 0) return true;
  return false;
}

LiteRfidAction lite_rfid_decide(const LiteRfidDecideIn &in)
{
  if (!in.enabled)            return LiteRfidAction::None;   // not gating
  if (!in.present)            return LiteRfidAction::None;   // no card this cycle
  if (in.already_authorized)  return LiteRfidAction::None;   // ignore duplicate swipe
  return in.allowed ? LiteRfidAction::Authorize : LiteRfidAction::Deny;
}
