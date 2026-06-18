#pragma once
#include <stddef.h>
#include <stdint.h>

// Pure RFID policy: allow-list parse/match + the swipe->charge decision, plus the
// always-compiled live status struct the web layer reads. No hardware, no Arduino —
// compiled into the native test env and the device build alike.

#define LITE_RFID_MAX_ALLOW   16
#define LITE_RFID_UID_MAXLEN  10
#define LITE_RFID_UID_HEX_MAX (2 * LITE_RFID_UID_MAXLEN + 1)   // 21: 20 hex + NUL

// Parsed allow-list: normalized uppercase UID hex strings (no separators).
struct LiteRfidAllowlist {
  char    list[LITE_RFID_MAX_ALLOW][LITE_RFID_UID_HEX_MAX];
  uint8_t count;
};

// Parse a delimited allow-list string into normalized entries. Accepts ',', ';',
// whitespace, and newlines as separators. Entries are upper-cased; malformed tokens
// (odd length, non-hex, too long) are skipped. Returns the count stored (<= MAX).
uint8_t lite_rfid_allowlist_parse(const char *text, LiteRfidAllowlist *out);

// Case-insensitive membership test. Empty list or empty uid -> false.
bool lite_rfid_uid_allowed(const char *uid_hex, const LiteRfidAllowlist *allow);

// What the reader loop should do with this cycle's scan result.
enum class LiteRfidAction : uint8_t {
  None = 0,    // no change (feature off, no card, or duplicate of an active auth)
  Authorize,   // a listed tag was presented -> enable charging for the session
  Deny,        // an unlisted tag was presented -> keep charging blocked, flag it
};

struct LiteRfidDecideIn {
  bool enabled;             // RFID gating switched on
  bool present;             // a UID was read this cycle
  bool allowed;             // that UID is on the allow-list
  bool already_authorized;  // a session is already RFID-authorized
};

// Swipe-to-authorize semantics (mirrors OpenEVSE): a listed tag authorizes the
// session; an unlisted tag is denied/logged; a duplicate swipe of the already-active
// tag is ignored (no accidental stop). Session-end release is the caller's job.
LiteRfidAction lite_rfid_decide(const LiteRfidDecideIn &in);

// Live reader status, surfaced in /status. Always compiled (native + device); the
// device glue (lite_rfid.cpp) updates it, the web layer reads it. Defaults describe
// "no reader present" so a build without RFID wired reports honestly.
struct LiteRfidStatus {
  bool     enabled;
  bool     reader_ok;        // selfTest() passed at begin()
  uint32_t failure_count;    // cumulative read/self-test failures
  bool     authorized;       // a session is RFID-authorized right now
  bool     last_allowed;     // was last_uid on the allow-list?
  uint32_t last_scan_ms;     // millis() of the last successful scan (0 = never)
  char     last_uid[LITE_RFID_UID_HEX_MAX];   // most recent UID ("" = none)
};

extern LiteRfidStatus g_lite_rfid_status;

void lite_rfid_status_reset(LiteRfidStatus *s);
