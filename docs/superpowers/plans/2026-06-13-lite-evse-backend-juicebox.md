# Lite Modular EVSE Backend + JuiceBox `$`-Protocol Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the lite firmware a pluggable `LiteEvseBackend` and its first impl — a JuiceBox `$`-protocol backend that decodes the MCU's status into `/status` and keeps the MCU online with a heartbeat (read + heartbeat only; no charge-current control).

**Architecture:** A pure, host-tested `juicebox_proto` module (`$`-delimited frame parser + `$ES` decode + frame/heartbeat builders) sits under a thin abstract `LiteEvseBackend` interface. `JuiceBoxBackend` implements the interface over `Serial`; `main_lite`/`web_server_lite` depend only on the interface. Exactly one backend is compiled per board via `-D LITE_EVSE_BACKEND_JUICEBOX`. The stock OpenEVSE/RAPI stack (`EvseManager`/`OpenEVSEClass`/`RapiSender`) is dropped from the lite env.

**Tech Stack:** C++17, PlatformIO, doctest (native host tests), LibreTiny/Arduino (WGM160P), Mongoose (existing web server). Source of truth for the protocol: `/home/rar/device-configs/esphome/juicebox/stock/juicebox_atmega_flash.bin` (32 KB AVR/ATmega flash dump).

**Spec:** `docs/superpowers/specs/2026-06-12-lite-evse-backend-juicebox-design.md`

---

## File structure

| File | Responsibility |
|------|----------------|
| `src/lite/lite_evse_state.h` | Canonical `LiteEvseState` enum (backend-agnostic). Pure. |
| `src/lite/juicebox_proto.h` / `.cpp` | `$`-delimited frame parser, `$ES` decode, JB→canonical state map, frame + heartbeat builders. Pure (no Arduino/ArduinoJson). |
| `src/lite/lite_evse_backend.h` | Abstract `LiteEvseBackend` interface. Includes `lite_evse_state.h` + ArduinoJson. |
| `src/lite/juicebox_backend.h` / `.cpp` | `LiteEvseBackend` impl: owns `Serial`, pumps the parser, holds latest state + `lastRxMillis`, sends heartbeat. Gated `#if defined(OPENEVSE_LITE) && defined(LITE_EVSE_BACKEND_JUICEBOX)`. |
| `src/lite/main_lite.cpp` | Instantiate the selected backend; pass to web server. |
| `src/lite/web_server_lite.cpp` / `.h` | `/status` reads `LiteEvseBackend&` instead of `EvseManager&`. |
| `test/test_juicebox_proto/test_juicebox_proto.cpp` | Native doctest suite for `juicebox_proto`. |
| `docs/superpowers/notes/2026-06-13-juicebox-protocol-re.md` | RE findings (Task 1): heartbeat bytes, offline timeout, `$ES` field/state codes. |
| `platformio.ini` | `[env:native]` adds the proto source; `[env:openevse_lite]` drops EVSE-core sources/lib_deps and adds the backend + `-D LITE_EVSE_BACKEND_JUICEBOX`. |

---

## Task 1: Reverse-engineer the heartbeat / offline contract + state codes

This is an **investigation task** (not TDD). It produces a committed findings note and a constants block other tasks depend on. Do it first — Tasks 4 and 3 consume its values.

**Files:**
- Create: `docs/superpowers/notes/2026-06-13-juicebox-protocol-re.md`
- Reference: `/home/rar/device-configs/esphome/juicebox/stock/juicebox_atmega_flash.bin`

- [ ] **Step 1: Disassemble the ATmega flash**

Run:
```bash
# avr-objdump ships with the PlatformIO atmelavr toolchain, or system avr-binutils.
AVROBJ=$(ls ~/.platformio/packages/toolchain-atmelavr/bin/avr-objdump 2>/dev/null || command -v avr-objdump)
"$AVROBJ" -D -m avr5 -b binary \
  /home/rar/device-configs/esphome/juicebox/stock/juicebox_atmega_flash.bin \
  > /tmp/jb_flash.asm
wc -l /tmp/jb_flash.asm
```
Expected: a multi-thousand-line AVR disassembly.

- [ ] **Step 2: Locate the protocol strings and their xrefs**

Run:
```bash
strings -t d -n 3 /home/rar/device-configs/esphome/juicebox/stock/juicebox_atmega_flash.bin \
  | grep -E '\$WC|\$WR|_L amps|offline|Should be|\$ES|message length'
```
Note the byte offsets. For each of `$WC`, `$WR`, the `_L amps` error, and `"No comm signal"`, find where the firmware references that string/handler in `/tmp/jb_flash.asm` (search for the address, and for the RX-parser dispatch that compares incoming type bytes against `'W','C'` / `'W','R'`).

- [ ] **Step 3: Determine the heartbeat and offline timeout**

From the RX parser and the "offline mode" branch, answer and record:
1. **Frame delimiter:** is each message terminated by `\r`, `\n`, both, or only self-delimited by the leading `$`? (Confirms the parser's flush rule.)
2. **Heartbeat:** exactly which inbound frame(s) reset the "no comm signal" watchdog. Capture the literal bytes (e.g. `$WC000:` vs `$WC___:` with a payload). If the keep-alive carries an amps setpoint, record its format and the safe no-op value (echo the MCU's reported amps).
3. **Offline timeout:** the watchdog period (seconds) after which the MCU goes offline. The heartbeat interval must be comfortably below this (target ≤ half).

- [ ] **Step 4: Decode `$ES` `S` (state) and `F` (fault) codes**

From the `$ES`-builder code paths (search the disassembly near the `$ES` string xref and the GFI/diode-fault branches), record the integer values for: not-connected, connected/ready, charging, and error/GFI states (`S` field), and known `F` fault codes (e.g. the `008:Ground Fault Int Lockout` / `101:Ground Fault Int` strings). Record what `L` and `H` represent if discoverable; otherwise mark them "stored raw, unmapped".

- [ ] **Step 5: Write the findings note**

Write `docs/superpowers/notes/2026-06-13-juicebox-protocol-re.md` documenting Steps 3–4 concretely: the delimiter, the exact heartbeat byte string, the timeout, the heartbeat interval to use, the `S`/`F` code tables, and `L`/`H` status. This note is the authority Tasks 3 and 4 cite.

- [ ] **Step 6: Commit**

```bash
cd /home/rar/oevse/openevse-juicebox-lite
git add docs/superpowers/notes/2026-06-13-juicebox-protocol-re.md
git commit -m "re(lite): document JuiceBox \$-protocol heartbeat, offline timeout, state/fault codes"
```

> **If the disassembly cannot resolve the heartbeat** (obfuscated/indirect), fall back to a live UART capture of a stock WiFi module against this MCU. Do NOT transmit guessed bytes to the MCU on real hardware before the heartbeat is confirmed — an unexpected command makes the MCU log `"received when not expecting it"` and could mis-sequence state.

---

## Task 2: `juicebox_proto` — `$`-delimited frame parser (pure, TDD)

**Files:**
- Create: `src/lite/juicebox_proto.h`, `src/lite/juicebox_proto.cpp`, `src/lite/lite_evse_state.h`
- Create: `test/test_juicebox_proto/test_juicebox_proto.cpp`
- Modify: `platformio.ini` `[env:native]` `build_src_filter`

- [ ] **Step 1: Add the canonical state enum**

Create `src/lite/lite_evse_state.h`:
```cpp
#pragma once
#include <stdint.h>

// Backend-agnostic EVSE state. Each backend maps its native codes onto this.
enum class LiteEvseState : uint8_t {
  Unknown = 0,
  NotConnected,   // no vehicle on the pilot
  Connected,      // vehicle present, not charging
  Charging,
  Error,          // fault / GFI / lockout
};
```

- [ ] **Step 2: Declare the proto API**

Create `src/lite/juicebox_proto.h`:
```cpp
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "lite_evse_state.h"

static const size_t JB_TYPE_LEN    = 2;
static const size_t JB_MAX_PAYLOAD = 80;

// One decoded protocol frame: 2-char type + NUL-terminated payload.
struct JuiceBoxFrame {
  char     type[JB_TYPE_LEN + 1];
  char     payload[JB_MAX_PAYLOAD + 1];
  uint16_t len;                       // payload length
};

// Decoded $ES status fields (raw JB values, pre-normalization).
struct JuiceBoxStatus {
  bool valid;
  int  state, line, temp, h, amps, power, fault;
};

// Split a frame body (everything AFTER the leading '$') into type + payload.
// Handles "<TT><3hex>:<payload>" (e.g. $ES01C:...) and "<TT>:<payload>" (e.g. $MD:...).
bool juicebox_parse_frame_body(const char *raw, size_t n, JuiceBoxFrame &out);

// Incremental, framing-tolerant parser. A frame runs from a '$' to the next '$'
// or a CR/LF terminator (whichever comes first). Resyncs on '$'. feed() returns
// true and fills `out` exactly once per completed frame.
class JuiceBoxParser {
public:
  JuiceBoxParser() : _n(0), _started(false) {}
  bool feed(uint8_t b, JuiceBoxFrame &out);
  void reset() { _n = 0; _started = false; }
private:
  bool flush(JuiceBoxFrame &out);
  char   _raw[JB_MAX_PAYLOAD + 16];
  size_t _n;
  bool   _started;
};

// Decode a $ES payload (e.g. "S00,L00,T00,H00,A00,P000,F00") into JuiceBoxStatus.
bool juicebox_parse_es(const char *payload, uint16_t len, JuiceBoxStatus &out);

// Map a raw JB S-field code onto the canonical state (codes confirmed in Task 1).
LiteEvseState juicebox_map_state(int raw);

// Build "$<type><LLL hex>:<payload>" into buf. Returns bytes written, 0 on overflow.
size_t juicebox_build_frame(const char *type, const char *payload, char *buf, size_t buflen);

// Build the keep-alive heartbeat frame (exact bytes per Task 1). Returns bytes written.
size_t juicebox_build_heartbeat(char *buf, size_t buflen);
```

- [ ] **Step 3: Write the failing parser tests**

Create `test/test_juicebox_proto/test_juicebox_proto.cpp`:
```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../src/lite/juicebox_proto.h"
#include <string.h>

static bool feed_str(JuiceBoxParser &p, const char *s, JuiceBoxFrame &out) {
  bool got = false;
  for (const char *c = s; *c; ++c) { if (p.feed((uint8_t)*c, out)) got = true; }
  return got;
}

TEST_CASE("parses a structured $ES frame terminated by CR") {
  JuiceBoxParser p; JuiceBoxFrame f;
  REQUIRE(feed_str(p, "$ES01C:S00,L00,T00,H00,A00,P000,F00\r", f));
  CHECK(strcmp(f.type, "ES") == 0);
  CHECK(strcmp(f.payload, "S00,L00,T00,H00,A00,P000,F00") == 0);
  CHECK(f.len == 0x1C);
}

TEST_CASE("parses a length-less $MD debug frame") {
  JuiceBoxParser p; JuiceBoxFrame f;
  REQUIRE(feed_str(p, "$MD:Back Online\n", f));
  CHECK(strcmp(f.type, "MD") == 0);
  CHECK(strcmp(f.payload, "Back Online") == 0);
}

TEST_CASE("the next $ flushes the previous frame (no terminator needed)") {
  JuiceBoxParser p; JuiceBoxFrame f;
  // First frame completes when the second '$' arrives.
  bool got = false;
  for (const char *c = "$PV002:20$ES"; *c; ++c) if (p.feed((uint8_t)*c, f)) got = true;
  REQUIRE(got);
  CHECK(strcmp(f.type, "PV") == 0);
  CHECK(strcmp(f.payload, "20") == 0);
}

TEST_CASE("garbage before '$' is discarded (resync)") {
  JuiceBoxParser p; JuiceBoxFrame f;
  REQUIRE(feed_str(p, "xyz\x00\xff$FW006:100102\r", f));
  CHECK(strcmp(f.type, "FW") == 0);
  CHECK(strcmp(f.payload, "100102") == 0);
}

TEST_CASE("a frame split across feeds still parses") {
  JuiceBoxParser p; JuiceBoxFrame f;
  CHECK_FALSE(feed_str(p, "$ES01C:S00,L00,", f));   // partial, no flush yet
  REQUIRE(feed_str(p, "T00,H00,A00,P000,F00\r", f));
  CHECK(strcmp(f.payload, "S00,L00,T00,H00,A00,P000,F00") == 0);
}

TEST_CASE("an over-long runaway line does not overflow") {
  JuiceBoxParser p; JuiceBoxFrame f;
  char big[256]; big[0] = '$'; big[1] = 'E'; big[2] = 'S';
  memset(big + 3, 'A', sizeof(big) - 4); big[sizeof(big) - 1] = '\0';
  for (char *c = big; *c; ++c) p.feed((uint8_t)*c, f);  // must not crash
  CHECK(true);
}
```

- [ ] **Step 4: Run the tests to verify they fail**

Run: `pio test -e native -f test_juicebox_proto`
Expected: FAIL (link error / undefined `JuiceBoxParser::feed`, `juicebox_parse_frame_body`).

- [ ] **Step 5: Add the proto source to the native env**

In `platformio.ini`, edit the `[env:native]` `build_src_filter` to append the new source:
```ini
build_src_filter = -<*> +<tsdb_sample.cpp> +<home_battery.cpp> +<lite/lite_random.cpp> +<lite/juicebox_proto.cpp>
```

- [ ] **Step 6: Implement the parser**

Create `src/lite/juicebox_proto.cpp` (parser + frame-body split; other functions land in Tasks 3–4):
```cpp
#include "juicebox_proto.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static bool is_hex(char c) {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

bool juicebox_parse_frame_body(const char *raw, size_t n, JuiceBoxFrame &out) {
  memset(&out, 0, sizeof(out));
  if (n < 3) return false;                  // need 2 type chars + at least ':'
  out.type[0] = raw[0];
  out.type[1] = raw[1];
  out.type[2] = '\0';

  const char *payload = nullptr;
  if (n >= 6 && is_hex(raw[2]) && is_hex(raw[3]) && is_hex(raw[4]) && raw[5] == ':') {
    payload = raw + 6;
    out.len = (uint16_t)strtol(raw + 2, nullptr, 16);   // NB: strtol reads up to ':'
  } else if (raw[2] == ':') {
    payload = raw + 3;
  } else {
    const char *colon = strchr(raw + 2, ':');
    if (!colon) return false;
    payload = colon + 1;
  }

  size_t plen = strlen(payload);
  if (plen > JB_MAX_PAYLOAD) plen = JB_MAX_PAYLOAD;
  memcpy(out.payload, payload, plen);
  out.payload[plen] = '\0';
  if (out.len == 0) out.len = (uint16_t)plen;
  return true;
}

bool JuiceBoxParser::flush(JuiceBoxFrame &out) {
  if (!_started || _n == 0) { _started = false; _n = 0; return false; }
  _raw[_n] = '\0';
  bool ok = juicebox_parse_frame_body(_raw, _n, out);
  _started = false; _n = 0;
  return ok;
}

bool JuiceBoxParser::feed(uint8_t b, JuiceBoxFrame &out) {
  if (b == '$') {
    bool ready = flush(out);   // close any in-progress frame
    _started = true; _n = 0;
    return ready;
  }
  if (b == '\r' || b == '\n') {
    return flush(out);
  }
  if (_started) {
    if (_n < sizeof(_raw) - 1) _raw[_n++] = (char)b;
    else { _started = false; _n = 0; }   // runaway → drop, resync on next '$'
  }
  return false;
}
```

- [ ] **Step 7: Run the tests to verify they pass**

Run: `pio test -e native -f test_juicebox_proto`
Expected: PASS (6 cases).

- [ ] **Step 8: Commit**

```bash
cd /home/rar/oevse/openevse-juicebox-lite
git add src/lite/lite_evse_state.h src/lite/juicebox_proto.h src/lite/juicebox_proto.cpp \
        test/test_juicebox_proto/test_juicebox_proto.cpp platformio.ini
git commit -m "feat(lite): JuiceBox \$-delimited frame parser + canonical state enum (host-tested)"
```

---

## Task 3: `$ES` decode + canonical state map (pure, TDD)

**Files:**
- Modify: `src/lite/juicebox_proto.cpp` (add `juicebox_parse_es`, `juicebox_map_state`)
- Modify: `test/test_juicebox_proto/test_juicebox_proto.cpp`

- [ ] **Step 1: Write the failing tests**

Append to `test/test_juicebox_proto/test_juicebox_proto.cpp`:
```cpp
TEST_CASE("decodes all $ES fields including the 3-digit power field") {
  JuiceBoxStatus s;
  REQUIRE(juicebox_parse_es("S02,L01,T31,H00,A24,P240,F00", 27, s));
  CHECK(s.valid);
  CHECK(s.state == 2);
  CHECK(s.line  == 1);
  CHECK(s.temp  == 31);
  CHECK(s.amps  == 24);
  CHECK(s.power == 240);
  CHECK(s.fault == 0);
}

TEST_CASE("$ES decode tolerates a missing trailing field") {
  JuiceBoxStatus s;
  REQUIRE(juicebox_parse_es("S00,A00", 7, s));
  CHECK(s.state == 0);
  CHECK(s.amps  == 0);
}

TEST_CASE("$ES decode rejects empty payload") {
  JuiceBoxStatus s;
  CHECK_FALSE(juicebox_parse_es("", 0, s));
}

TEST_CASE("maps JB state codes to canonical states") {
  // Code values per docs/superpowers/notes/2026-06-13-juicebox-protocol-re.md (Task 1).
  CHECK(juicebox_map_state(0) == LiteEvseState::NotConnected);
  CHECK(juicebox_map_state(1) == LiteEvseState::Connected);
  CHECK(juicebox_map_state(2) == LiteEvseState::Charging);
  CHECK(juicebox_map_state(99) == LiteEvseState::Unknown);
}
```

> Adjust the four `juicebox_map_state` expectations to the actual code values recorded in the Task 1 note before implementing. The fault/error mapping is driven by the `$ES` `F` field (non-zero ⇒ `Error`) in the backend, not by `S`.

- [ ] **Step 2: Run to verify failure**

Run: `pio test -e native -f test_juicebox_proto`
Expected: FAIL (undefined `juicebox_parse_es` / `juicebox_map_state`).

- [ ] **Step 3: Implement the decoder and mapper**

Append to `src/lite/juicebox_proto.cpp`:
```cpp
bool juicebox_parse_es(const char *payload, uint16_t len, JuiceBoxStatus &out) {
  memset(&out, 0, sizeof(out));
  if (!payload || len == 0) return false;

  const char *p   = payload;
  const char *end = payload + len;
  int fields = 0;

  while (p < end && *p) {
    char f = *p++;                       // field letter
    bool neg = false;
    if (p < end && *p == '-') { neg = true; ++p; }
    int v = 0; bool any = false;
    while (p < end && *p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); ++p; any = true; }
    if (!any) return false;              // letter with no number ⇒ malformed
    if (neg) v = -v;

    switch (f) {
      case 'S': out.state = v; break;
      case 'L': out.line  = v; break;
      case 'T': out.temp  = v; break;
      case 'H': out.h     = v; break;
      case 'A': out.amps  = v; break;
      case 'P': out.power = v; break;
      case 'F': out.fault = v; break;
      default: break;                    // ignore unknown field letters
    }
    ++fields;
    if (p < end && *p == ',') ++p;       // step over separator
  }
  out.valid = fields > 0;
  return out.valid;
}

LiteEvseState juicebox_map_state(int raw) {
  // Code values confirmed in the Task 1 RE note. Update both this table and the
  // matching test if the disassembly shows different values.
  switch (raw) {
    case 0:  return LiteEvseState::NotConnected;
    case 1:  return LiteEvseState::Connected;
    case 2:  return LiteEvseState::Charging;
    default: return LiteEvseState::Unknown;
  }
}
```

- [ ] **Step 4: Run to verify pass**

Run: `pio test -e native -f test_juicebox_proto`
Expected: PASS (all Task 2 + Task 3 cases).

- [ ] **Step 5: Commit**

```bash
cd /home/rar/oevse/openevse-juicebox-lite
git add src/lite/juicebox_proto.cpp test/test_juicebox_proto/test_juicebox_proto.cpp
git commit -m "feat(lite): decode JuiceBox \$ES fields + map S-code to canonical state"
```

---

## Task 4: Frame + heartbeat builders (pure, TDD)

**Files:**
- Modify: `src/lite/juicebox_proto.cpp` (add `juicebox_build_frame`, `juicebox_build_heartbeat`)
- Modify: `test/test_juicebox_proto/test_juicebox_proto.cpp`

- [ ] **Step 1: Write the failing tests**

Append to `test/test_juicebox_proto/test_juicebox_proto.cpp`:
```cpp
TEST_CASE("build_frame emits $<type><3hex len>:<payload>") {
  char buf[64];
  size_t n = juicebox_build_frame("PV", "20", buf, sizeof(buf));
  REQUIRE(n > 0);
  CHECK(strcmp(buf, "$PV002:20") == 0);
}

TEST_CASE("build_frame round-trips through the parser") {
  char buf[64];
  REQUIRE(juicebox_build_frame("ES", "S00,A00", buf, sizeof(buf)) > 0);
  JuiceBoxFrame f;
  // Append a terminator so the parser flushes.
  char line[80]; snprintf(line, sizeof(line), "%s\r", buf);
  JuiceBoxParser p; bool got = false;
  for (char *c = line; *c; ++c) if (p.feed((uint8_t)*c, f)) got = true;
  REQUIRE(got);
  CHECK(strcmp(f.type, "ES") == 0);
  CHECK(strcmp(f.payload, "S00,A00") == 0);
}

TEST_CASE("build_frame refuses an undersized buffer") {
  char buf[4];
  CHECK(juicebox_build_frame("PV", "20", buf, sizeof(buf)) == 0);
}

TEST_CASE("heartbeat builder emits the confirmed keep-alive bytes") {
  char buf[32];
  size_t n = juicebox_build_heartbeat(buf, sizeof(buf));
  REQUIRE(n > 0);
  // EXACT bytes per docs/.../2026-06-13-juicebox-protocol-re.md (Task 1).
  CHECK(strcmp(buf, "$WC000:") == 0);
}
```

> Replace the `"$WC000:"` expectation with the literal heartbeat string recorded in the Task 1 note before implementing `juicebox_build_heartbeat`.

- [ ] **Step 2: Run to verify failure**

Run: `pio test -e native -f test_juicebox_proto`
Expected: FAIL (undefined `juicebox_build_frame` / `juicebox_build_heartbeat`).

- [ ] **Step 3: Implement the builders**

Append to `src/lite/juicebox_proto.cpp`:
```cpp
size_t juicebox_build_frame(const char *type, const char *payload, char *buf, size_t buflen) {
  if (!type || strlen(type) != JB_TYPE_LEN) return 0;
  size_t plen = payload ? strlen(payload) : 0;
  if (plen > 0xFFF) return 0;
  size_t need = 1 + JB_TYPE_LEN + 3 + 1 + plen + 1;   // $ TT LLL : payload NUL
  if (buflen < need) return 0;
  int n = snprintf(buf, buflen, "$%s%03X:%s", type, (unsigned)plen, payload ? payload : "");
  return (n > 0 && (size_t)n < buflen) ? (size_t)n : 0;
}

size_t juicebox_build_heartbeat(char *buf, size_t buflen) {
  // Keep-alive that resets the MCU "no comm signal" watchdog. Bytes per Task 1.
  return juicebox_build_frame("WC", "", buf, buflen);
}
```

- [ ] **Step 4: Run to verify pass**

Run: `pio test -e native -f test_juicebox_proto`
Expected: PASS (all proto cases).

- [ ] **Step 5: Commit**

```bash
cd /home/rar/oevse/openevse-juicebox-lite
git add src/lite/juicebox_proto.cpp test/test_juicebox_proto/test_juicebox_proto.cpp
git commit -m "feat(lite): JuiceBox frame + heartbeat builders (host-tested round-trip)"
```

---

## Task 5: `LiteEvseBackend` interface + `JuiceBoxBackend` device impl

Device-only glue (no host test; verified by the Task 6 build + Task 7 HW run). Keep behaviour thin — all parsing/decoding lives in the host-tested proto.

**Files:**
- Create: `src/lite/lite_evse_backend.h`, `src/lite/juicebox_backend.h`, `src/lite/juicebox_backend.cpp`

- [ ] **Step 1: Define the abstract interface**

Create `src/lite/lite_evse_backend.h`:
```cpp
#pragma once
#ifdef OPENEVSE_LITE
#include <ArduinoJson.h>
#include "lite_evse_state.h"

// Backend-agnostic EVSE device seam. web_server_lite + main_lite depend ONLY on this.
class LiteEvseBackend {
public:
  virtual ~LiteEvseBackend() {}
  virtual void begin() = 0;
  virtual void loop()  = 0;

  virtual bool          isOnline() const = 0;
  virtual LiteEvseState getState() const = 0;
  virtual int           getAmps()  const = 0;
  virtual int           getPower() const = 0;
  virtual int           getTemp()  const = 0;
  virtual int           getFault() const = 0;

  // Backend-specific extras (identity strings, raw fields, …).
  virtual void addStatusFields(JsonDocument &doc) const = 0;
};
#endif // OPENEVSE_LITE
```

- [ ] **Step 2: Declare `JuiceBoxBackend`**

Create `src/lite/juicebox_backend.h`:
```cpp
#pragma once
#if defined(OPENEVSE_LITE) && defined(LITE_EVSE_BACKEND_JUICEBOX)
#include <Arduino.h>
#include "lite_evse_backend.h"
#include "juicebox_proto.h"

class JuiceBoxBackend : public LiteEvseBackend {
public:
  explicit JuiceBoxBackend(Stream &port) : _port(port) {}

  void begin() override;
  void loop()  override;

  bool          isOnline() const override;
  // A non-zero $ES F (fault) field takes precedence over the S state code.
  LiteEvseState getState() const override {
    return _status.fault != 0 ? LiteEvseState::Error : juicebox_map_state(_status.state);
  }
  int           getAmps()  const override { return _status.amps; }
  int           getPower() const override { return _status.power; }
  int           getTemp()  const override { return _status.temp; }
  int           getFault() const override { return _status.fault; }
  void          addStatusFields(JsonDocument &doc) const override;

private:
  void handleFrame(const JuiceBoxFrame &f);
  void sendHeartbeat();

  Stream        &_port;
  JuiceBoxParser _parser;
  JuiceBoxStatus _status = {};
  unsigned long  _lastRxMillis   = 0;
  unsigned long  _lastBeatMillis = 0;
  bool           _everRx         = false;
  char           _hw[24] = {0};
  char           _fw[16] = {0};
  char           _pv[8]  = {0};
  char           _md[48] = {0};
};
#endif
```

- [ ] **Step 3: Implement `JuiceBoxBackend`**

Create `src/lite/juicebox_backend.cpp`:
```cpp
#if defined(OPENEVSE_LITE) && defined(LITE_EVSE_BACKEND_JUICEBOX)
#include "juicebox_backend.h"
#include <string.h>

// Heartbeat cadence and offline window per docs/.../2026-06-13-juicebox-protocol-re.md.
// Both are comfortably under the MCU's watchdog (Task 1). Adjust to the recorded values.
static const unsigned long JB_HEARTBEAT_INTERVAL_MS = 1000;
static const unsigned long JB_OFFLINE_TIMEOUT_MS    = 5000;

void JuiceBoxBackend::begin() {
  _lastBeatMillis = millis();
}

void JuiceBoxBackend::loop() {
  JuiceBoxFrame f;
  while (_port.available() > 0) {
    if (_parser.feed((uint8_t)_port.read(), f)) {
      _lastRxMillis = millis();
      _everRx = true;
      handleFrame(f);
    }
  }

  // Only transmit once the MCU has proven it's there (don't blast a silent line).
  unsigned long now = millis();
  if (_everRx && (now - _lastBeatMillis) >= JB_HEARTBEAT_INTERVAL_MS) {
    sendHeartbeat();
    _lastBeatMillis = now;
  }
}

bool JuiceBoxBackend::isOnline() const {
  return _everRx && (millis() - _lastRxMillis) < JB_OFFLINE_TIMEOUT_MS;
}

void JuiceBoxBackend::handleFrame(const JuiceBoxFrame &f) {
  if      (!strcmp(f.type, "ES")) { juicebox_parse_es(f.payload, f.len, _status); }
  else if (!strcmp(f.type, "HW")) { strncpy(_hw, f.payload, sizeof(_hw) - 1); }
  else if (!strcmp(f.type, "FW")) { strncpy(_fw, f.payload, sizeof(_fw) - 1); }
  else if (!strcmp(f.type, "PV")) { strncpy(_pv, f.payload, sizeof(_pv) - 1); }
  else if (!strcmp(f.type, "MD")) { strncpy(_md, f.payload, sizeof(_md) - 1); }
  // other types ignored this slice
}

void JuiceBoxBackend::sendHeartbeat() {
  char buf[32];
  size_t n = juicebox_build_heartbeat(buf, sizeof(buf));
  if (n) _port.write((const uint8_t *)buf, n);
}

void JuiceBoxBackend::addStatusFields(JsonDocument &doc) const {
  if (_hw[0]) doc["hw"]       = _hw;
  if (_fw[0]) doc["fw"]       = _fw;
  if (_pv[0]) doc["protocol"] = _pv;
  if (_md[0]) doc["md"]       = _md;
  doc["line"] = _status.line;   // raw JB L field (semantics per Task 1)
}
#endif
```

- [ ] **Step 4: Commit**

```bash
cd /home/rar/oevse/openevse-juicebox-lite
git add src/lite/lite_evse_backend.h src/lite/juicebox_backend.h src/lite/juicebox_backend.cpp
git commit -m "feat(lite): LiteEvseBackend interface + JuiceBoxBackend (Serial, heartbeat, /status feed)"
```

---

## Task 6: Wire the backend into main + web + build; drop the RAPI stack

**Files:**
- Modify: `src/lite/main_lite.cpp`
- Modify: `src/lite/web_server_lite.h`, `src/lite/web_server_lite.cpp`
- Modify: `platformio.ini` `[env:openevse_lite]`
- Remove from build: `lite_evse_stubs.cpp` (no longer referenced)

- [ ] **Step 1: Switch the web server to the backend interface**

In `src/lite/web_server_lite.h`, replace the `EvseManager` forward/param with `LiteEvseBackend`:
```cpp
#pragma once
#ifdef OPENEVSE_LITE
class LiteEvseBackend;
void web_server_lite_begin(LiteEvseBackend &backend);
void web_server_lite_loop();
#endif
```

In `src/lite/web_server_lite.cpp`: replace the `#include "evse_man.h"` with `#include "lite_evse_backend.h"`, rename `s_evse`/param to `s_backend` (type `LiteEvseBackend *`), and rewrite `build_status_json`:
```cpp
static void build_status_json(String &out)
{
  StaticJsonDocument<256> doc;
  if (s_backend) {
    doc["state"]  = (int)s_backend->getState();
    doc["amp"]    = s_backend->getAmps();
    doc["power"]  = s_backend->getPower();
    doc["temp"]   = s_backend->getTemp();
    doc["fault"]  = s_backend->getFault();
    doc["online"] = s_backend->isOnline() ? 1 : 0;
    s_backend->addStatusFields(doc);
  }
  doc["free_heap"] = ESPAL.getFreeHeap();
  doc["uptime"]    = (uint32_t)(millis() / 1000);
  serializeJson(doc, out);
}
```
Update `web_server_lite_begin(LiteEvseBackend &backend)` to stash `s_backend = &backend;`. Drop the `#include "evse_man.h"` and the now-unused mongoose-before-C++ note lines that reference RapiSender.

- [ ] **Step 2: Rewrite `main_lite.cpp` to construct the selected backend**

Replace the `EvseManager`/`debug.h`/`event_log.h`/`evse_man.h` includes and globals in `src/lite/main_lite.cpp` with the backend selection:
```cpp
#ifdef OPENEVSE_LITE
#include <Arduino.h>
#include <WiFi.h>

#include "espal_lite.h"
#include "web_server_lite.h"
#include "lite_evse_backend.h"

#if defined(LITE_EVSE_BACKEND_JUICEBOX)
#include "juicebox_backend.h"
static JuiceBoxBackend s_backend(Serial);   // USART0 LOC1 (PE7=TX/PE6=RX) @ 9600 8N1
#else
#error "No lite EVSE backend selected (define LITE_EVSE_BACKEND_*)"
#endif

#ifndef LITE_WIFI_SSID
#define LITE_WIFI_SSID LITE_WIFI_SSID_DEFAULT
#endif
#ifndef LITE_WIFI_PASS
#define LITE_WIFI_PASS LITE_WIFI_PASS_DEFAULT
#endif

void setup()
{
  Serial.begin(9600);             // the JuiceBox $-protocol line (no debug prints here)
  ESPAL.begin();

  WiFi.begin(LITE_WIFI_SSID, LITE_WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
  }

  s_backend.begin();
  web_server_lite_begin(s_backend);
}

void loop()
{
  web_server_lite_loop();
  s_backend.loop();
}
#endif
```
(Drop `debug_setup()`/`MicroTask.update()`/`evse.getSender().loop()` — the StreamSpy/RAPI/MicroTasks path is gone. `debug_lite.cpp` is no longer needed but harmless if it stays excluded; remove it from the build in Step 3.)

- [ ] **Step 3: Update the lite env build filter, flags, and lib_deps**

In `platformio.ini` `[env:openevse_lite]`, set `build_src_filter` to the backend set (no EVSE-core sources, no stubs, no debug_lite/StreamSpy):
```ini
build_src_filter =
  -<*>
  +<lite/>
  -<lite/spike_main.cpp>
  -<lite/lite_config_store.cpp>
  -<lite/lite_evse_stubs.cpp>
  -<lite/debug_lite.cpp>
```
Remove the OpenEVSE/StreamSpy lib_deps lines (`jeremypoulter/OpenEVSE@0.0.15`, `jeremypoulter/StreamSpy@0.0.2`) and the `Micro Debug` dep; keep `ArduinoJson` and `MicroTasks` only if still referenced (MicroTasks is no longer needed once `EvseManager` is gone — drop it too if the build links clean without it). Add the backend selector to `build_flags`:
```ini
  -D LITE_EVSE_BACKEND_JUICEBOX
```

- [ ] **Step 4: Build the device firmware**

Run: `pio run -e openevse_lite`
Expected: SUCCESS. Note the flash %; it should be **lower** than the pre-change 24.4% (RAPI client + StreamSpy dropped). If the linker complains about a missing `divert`/`shaper`/`event_send` symbol, confirm `evse_man.cpp`/`evse_monitor.cpp` are truly out of the filter (they reference those) — they must not be compiled this slice.

- [ ] **Step 5: Re-run the native suite (no regressions)**

Run: `pio test -e native`
Expected: PASS (all envs, including `test_juicebox_proto`).

- [ ] **Step 6: Commit**

```bash
cd /home/rar/oevse/openevse-juicebox-lite
git add src/lite/main_lite.cpp src/lite/web_server_lite.h src/lite/web_server_lite.cpp platformio.ini
git commit -m "feat(lite): wire JuiceBoxBackend into main+/status; drop RAPI/EvseManager stack"
```

---

## Task 7: On-device hardware validation

**Files:** none (validation only). Uses `scripts/lite_flash.sh`.

- [ ] **Step 1: Build + flash with real WiFi creds**

Run (creds sourced from `~/secrets.yaml`; never echo them):
```bash
cd /home/rar/oevse/openevse-juicebox-lite
SSID=$(grep '^wifi_ssid:'     ~/secrets.yaml | cut -d'"' -f2)
PASS=$(grep '^wifi_password:' ~/secrets.yaml | cut -d'"' -f2)
echo "ssid_len=${#SSID} pass_len=${#PASS}"   # sanity, no secrets printed
PLATFORMIO_BUILD_FLAGS="-DLITE_WIFI_SSID=\\\"$SSID\\\" -DLITE_WIFI_PASS=\\\"$PASS\\\"" \
  pio run -e openevse_lite -t upload
```
Expected: `Verified OK` for both `bootloader.bin@0x0` and `firmware.bin@0x8000`; the flasher erases stale OTA metadata so it boots bank A.

- [ ] **Step 2: Confirm the board serves live JuiceBox state**

Run (board lands on its DHCP address — substitute the observed IP):
```bash
curl -s http://<board-ip>/status
```
Expected JSON includes `"online":1`, a plausible `"state"` (canonical int), and `amp`/`power`/`temp`/`fault` reflecting the MCU — plus `fw`/`hw`/`protocol` identity once those frames arrive. `state` should change when the vehicle/charge state changes.

- [ ] **Step 3: Confirm the heartbeat holds the MCU online**

Watch the `md` field across a few `/status` polls over ~30 s. It must **not** settle on `"No comm signal. Switching to offline mode"`. If it does, the heartbeat bytes/interval are wrong — return to Task 1, fix `juicebox_build_heartbeat` / `JB_HEARTBEAT_INTERVAL_MS`, and re-flash. (Do not increase the interval above the Task 1 timeout.)

- [ ] **Step 4: Record the result**

Append a short outcome (flash %, observed `/status`, online-hold confirmed) to `docs/superpowers/notes/2026-06-13-juicebox-protocol-re.md` and commit:
```bash
cd /home/rar/oevse/openevse-juicebox-lite
git add docs/superpowers/notes/2026-06-13-juicebox-protocol-re.md
git commit -m "docs(lite): record JuiceBox backend HW validation (read + heartbeat)"
```

---

## Notes for the implementer

- **Task 1 gates Tasks 3, 4, 7.** Its recorded values (heartbeat bytes, interval, offline timeout, `S`/`F` codes, frame delimiter) replace the best-estimate constants/expectations carried in those tasks. The parser (Task 2) is delimiter-tolerant by design and does **not** depend on Task 1.
- **Safety:** nothing in this plan sends an amps-set command. `juicebox_build_heartbeat` is the only transmit path, and it's gated on `_everRx`. Do not add a charge-current call this slice.
- **Secrets:** real WiFi creds only ever arrive via `PLATFORMIO_BUILD_FLAGS` from `~/secrets.yaml`; tracked files keep the `CHANGEME` placeholders. Never print the SSID/password (use lengths).
- **Commits:** no `Co-Authored-By` trailer. Commit per task; do not push unless asked.
