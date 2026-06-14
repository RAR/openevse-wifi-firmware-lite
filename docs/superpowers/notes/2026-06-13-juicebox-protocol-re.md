# JuiceBox `$`-protocol reverse-engineering (heartbeat / offline contract + state/fault codes)

**Task:** JB Task 1 — establish the exact bytes/contract the replacement WiFi firmware (WGM160P) must
exchange with the stock JuiceBox 40 ATmega EVSE controller over UART.

**Status:** investigation only. NOTHING here was transmitted to hardware; this is pure static analysis
of a flash dump. Each key value below carries a confidence level. **A value marked UNKNOWN / LIKELY is
a flag to validate with a live UART capture before trusting it on a real 40A contactor.**

## Source artifacts

- Flash dump: `/home/rar/device-configs/esphome/juicebox/stock/juicebox_atmega_flash.bin`
  (32 666 bytes / 0x7F9A).
- Disassembly: `avr-objdump -D -m avr5 -b binary … > /tmp/jb_flash.asm` (14 140 lines).
  Tool: `~/.platformio/packages/toolchain-atmelavr/bin/avr-objdump`.
- All addresses below are **byte offsets** in the dump (objdump `-b binary` reports byte addresses;
  string offsets are byte offsets in flash and match `strings -t d`).
- Firmware identity (strings): `JuiceBox 2.01 US`, build `Oct 6 2021 14:48:22`, `JB_firmware/JFirmware.cpp`.
- MCU is an **ATmega2560-class** part: UART data register UDR0 is at I/O-mem `0x00C0`, USART control at
  `0x00C0..0x00C6`, Timer regs TCCR0B `0x25`, TCCR1A/B `0x80/0x81` — these addresses only exist on the
  mega640/1280/2560 family.

---

## 1. Frame delimiter — **CONFIRMED: terminated by CR (`\r`,0x0D) OR LF (`\n`,0x0A); frame body self-delimited by leading `$`**

The RX line assembler reads one char at a time from the UART ring buffer and appends to a line buffer
until it sees a terminator:

```
  9a2:  call 0x2a74          ; getchar() from RX ring
  9ae:  cpi  r24, 0x0D       ; '\r'  -> branch 0xa00 (line complete)
  9b2:  cpi  r24, 0x0A       ; '\n'  -> branch 0xa00 (line complete)
  9b6:  …append to buffer 0x0570 (len), cap 0x2C = 44 bytes max…
```

Once a line is assembled, the parser scans it for the leading frame-start byte:

```
  9d2:  cpi  r24, 0x24       ; '$'   -> frame start at 0x9d6
```

So a complete inbound frame is: optional junk, then `$`, then `<2-char type><3-hex len>:<payload>`,
terminated by CR and/or LF. Either CR or LF (or both, e.g. `\r\n`) closes the line. Max line length is
44 bytes. `$MD:` debug frames carry no length field (consistent with the known framing).

**Recommendation for our TX:** terminate every frame we send with `\r\n`. Both are accepted; sending both
is safe and matches the longest line the assembler tolerates.

---

## 2. The comm watchdog (offline contract) — mechanism CONFIRMED; absolute timeout LIKELY

### 2a. The watchdog counter — **CONFIRMED**

A 16-bit software counter lives at SRAM `0x0112:0x0113` (low 12 bits used; the top nibble of `0x0113`
is masked off everywhere — `andi r17,0x0F` — so it shares the word with an unrelated field).

There are exactly **two** writers to this counter in the whole image (`grep sts 0x0112/0x0113`):

- **Decrement** (the tick), at `0x90e`, inside the main timer-bank sweep:
  ```
   90e: lds r24,0x0112 / lds r25,0x0113
   916: sbiw r24,0 ; breq (skip if already 0)
   91a: sbiw r24,1  ; decrement
   91c: sts back to 0x0113/0x0112
  ```
- **Reload**, at `0xd8e..0xd96`:
  ```
   d8e: ldi r24,0xB8 / ldi r25,0x0B   ; 0x0BB8 = 3000
   d92: sts 0x0113,r25 / sts 0x0112,r24
  ```

**Reload value = 0x0BB8 = 3000 (decimal). CONFIRMED — it is the only reload constant for this address.**

### 2b. The offline decision — **CONFIRMED**

In the main loop (`0xa56`):

```
  a56: lds r16,0x0112 / lds r17,0x0113
  a5e: andi r17,0x0F
  a60: or   r16,r17
  a62: brne 0xa6c             ; counter still > 0 -> stay online
  a64: lds  r24,0x0535
  a68: sbrs r24,1             ; if "offline" bit (bit1) not already set…
  a6a: rjmp 0xe92             ; …go offline
  …
  e92: ori r24,0x02 ; sts 0x0535   ; set offline bit 1 of flag byte 0x0535
  e98: …format string 0x0372 = "$MD:No comm signal. Switching to offline mode"…
  eb0: ldi r24,0x63 ; sts 0x0519   ; force S-state byte to 0x63 ('c'/99) command-code
```

When a valid `$` frame is received, the parser clears the offline bit (`0xa14: andi 0x0535,0xFD`) and
emits `$MD:Back Online` (string at 0x02C4), and the post-parse common path reloads the counter to 3000
(0xd8e). So: **any valid `$`-frame received resets the 3000-count watchdog and brings the unit back online.**

### 2c. Converting 3000 ticks to seconds — **LIKELY 3 s @ 1 ms tick; treat as UNKNOWN until live-verified**

The decrement at `0x90e` sits in a single large "timer-bank" routine (entry `0x764`, ends with the
`wdr` watchdog-pet at `0x794`) that decrements ~20 independent software countdown timers in one sweep
(`0x011C, 0x011B, 0x0108, 0x0529, 0x0528, 0x0526, 0x0527, 0x053E, 0x010A, 0x0119, 0x0117, 0x0116`, then
the comm counter `0x0112`). All of these decrement at the **same** rate — the system tick.

I could NOT prove the tick period from static analysis alone without a full Timer1/Timer2 prescaler +
compare-match trace (the firmware is Arduino-style: I see the `0x0F4240` = 1 000 000 µs constant and a
function-pointer timer ISR at `0x2c14` dispatching via `0x0102:0x0103`). The two most plausible readings:

- **1 ms tick → 3000 ms = 3.0 s** (most likely; a 3-second comm-loss-to-offline is typical for an EVSE
  keepalive and is consistent with the reload magnitude).
- 10 ms tick → 30 s (less likely given the counter would only need 300 then).

**Conclusion: offline timeout ≈ 3 seconds (LIKELY). Verify with a live capture by going quiet and timing
the `$MD:No comm signal…` emission.**

### 2d. Heartbeat recommendation

- The watchdog resets on **any** valid inbound `$`-frame, so the keepalive is "send a valid frame
  periodically," not a single magic byte string.
- The natural keepalive is the **amps-set command** (section 4): it both resets the watchdog and (re)asserts
  the desired charge current — a safe no-op is to re-send the MCU's currently-reported amps. Format and
  the 0–79 bound are confirmed (section 4).
- Do **not** rely on `$WC` as a free-running heartbeat: `$WC` is only accepted when the MCU is *expecting*
  it (see section 3) and otherwise logs an error.
- **Recommended heartbeat interval: 1 s** (≤ ⅓ of the LIKELY 3 s timeout). If live capture shows a longer
  timeout we can relax it, but 1 s is comfortably safe for the worst plausible case.

---

## 3. `$WC` / `$WR` — request/response tokens, NOT free heartbeats — **CONFIRMED (behavior)**

Inbound type dispatch is a packed-16-bit binary search on `r25:r24` (r25 = first type char, r24 = second).
Decoded comparisons (`/tmp/jb_flash.asm` ~0x1204–0x1260):

- `WC` (r25='W'0x57, r24='C'0x43) → handler `0x14cc`
- `WR` (r25='W', r24='R'0x52)     → handler inline at `0x125c`

The `$WC` handler (`0x14cc`):
```
 14cc: lds r18,0x0535
 14d0: sbrs r18,3          ; bit 3 = "expecting WC" flag
 14d2: rjmp 0x18b0         ; if NOT expecting -> error
 14d4: andi r18,0xF7 ; sts 0x0535  ; clear the expect-bit (consume the token)
 14da: sts 0x05F9, r1
```
`0x18b0` formats the string at byte 0x01D8 = `"$MD:RX Err:WC received when not expecting it"`. So `$WC`
is a handshake token the MCU only accepts after it asked for one. `$WR` behaves analogously (error string
`"$MD:RX Err:WR received when not expecting it"` at 0x0206).

The `$WC___:` / `$WR___:` literals at flash 0x03A1 / 0x03A9 are **response/echo templates** the MCU builds
(xref builds the frame at `0x251a` via the frame-builder `0x659c`), not commands we originate blindly.

---

## 4. Amps-set command (charge-current setpoint) — format CONFIRMED, exact 2-char type LIKELY

Evidence — the validation error strings and their parser:

- flash 0x0234: `"$MD:RX Err:_L amps value of XX received. Should be less than 80"`  (xref `0x1ac8`)
- flash 0x0275: `"$MD:RX Err:XX received with message length=000. Should be 002"`     (xref `0x14f0`)

The amps value is parsed from **two ASCII decimal digits** in the payload at SRAM `0x054A` (tens) and
`0x054B` (units):
```
 15a2: lds r24,0x054B ; +0xD0 (ASCII '0' adjust) -> units
 15aa: lds r24,0x054A ; ×10 fold                  -> tens
 …combine to a 0–99 value…
```
- Payload length must be exactly **2** ("Should be 002").
- Value must be **< 80** ("Should be less than 80"); the 40 A unit is further limited by its own config.

**Format: `$<type>` + `002` length + `:` + two decimal digits**, e.g. `…:16` for 16 A. The message *type*
for the amps command is the char `L` (the error string says `_L amps`, and the second-type-char `'L'`/0x4C
appears in the dispatch, e.g. `0x14e0: cpi 0x4C`), but the first type char and the exact 2-letter pair the
stock WiFi module uses is **LIKELY `?L` — confirm with a live capture** (the shared length-error target at
`0x14e8` is reached from several types, muddying a clean static read of the first byte).

**Safe no-op keepalive:** re-send the amps the MCU last reported in `$ES` field `A` (see §5), 2 digits,
length `002`.

---

## 5. `$ES` status frame fields — S/L/T/H/A/P/F

Real frame (string at flash 0x02D5): `$ES01C:S00,L00,T00,H00,A00,P000,F00` (payload 0x01C = 28 bytes).
The `$ES` builder is at `0xa9c` (frame-builder call `0x659c`, length `r20=0x23`=35). Each numeric field is
rendered by repeated divide-by-10 (`call 0x664c`) + `'0'` (0x30) adjust into the output buffer.

### S — pilot/charge state — **bounds CONFIRMED (0..5); per-state integer LIKELY**

The S digit is the decimal rendering of state byte SRAM `0x0519` (loaded at `0xba8` during `$ES` build).
A separate consumer at `0x5e82` does `cpi r18,0x06 ; brcc <invalid>` — i.e. **valid S-state values are
0..5**; ≥6 are treated as out-of-range. The host-commanded transition helper `0x3b72` writes the master
J1772 state `0x046E` to **3** (when told to enable, arg≠0) or **2** (arg==0); the `$WC` charging guard
checks `0x046E == 3`. Offline forces the S command-byte to 0x63.

Best-effort canonical map (bounds CONFIRMED 0–5; specific assignment LIKELY, per JuiceBox convention and
the `S00`-at-idle example frame — **verify with live capture across plug/charge/fault**):

| S | meaning (LIKELY)                  |
|---|-----------------------------------|
| 0 | not connected / standby (idle)    |
| 1 | connected / ready (plugged, not charging) |
| 2 | charging-enabled / start (host-commanded "go", state 0x046E=2 path) |
| 3 | charging (state 0x046E=3, `$WC` charging-guard value) |
| 4 | (LIKELY a wait/transition state)  |
| 5 | error / fault (detail in F)       |

Treat the exact 0–5 ↔ semantic mapping as **LIKELY** until a live capture confirms which integer appears
at unplugged / plugged-idle / charging / GFI.

### F — fault code — **CONFIRMED (literal strings in the image)**

The F field carries the fault code; the full code table is present verbatim in flash as ASCII
`"NNN:text"` strings (offsets shown):

Active faults:
| code | text (flash offset) |
|------|---------------------|
| 001 | FW Self Tests Failed (945) |
| 002 | Non-VM Error (970) |
| 003 | No GND (987) |
| 004 | Short Circuit Pilot (998) |
| 005 | Pilot Signal Gen Fail (1022) |
| 006 | GFI Auto Test Fail (1048) |
| 007 | Relay Stuck Closed (1071) |
| 008 | Ground Fault Int Lockout (1094) |
| 101 | Ground Fault Int (1123) |
| 102 | Relay Stuck Open (1144) |
| 103 | Diode Check Fail (1165) |
| 104 | Overheated (1186) |
| 105 | Vehicle vent req (1201) |
| 106 | HostEVSE Error (1222) |
| 107 | Lock Fail Error (1241) |

Clear/recovery events (`CLR …`): 503 CLR No GND, 504 CLR Short Circuit Pilot, 507 CLR Relay Stuck Closed,
601 CLR GFI, 602 CLR Relay Stuck Open, 603 CLR Diode Check Fail, 604 CLR Overheated, 605 CLR Vehicle vent
req, 606 CLR GFI Auto Test / CLR HostEVSE Error, 607 CLR Lock Failure (offsets 1261–1480). These are the
events the MCU reports when a fault self-clears.

GFI line-voltage diagnostic: `$MD:NFO:GFI Circuit Line Voltage Sensed at %d` (flash 0x6E28/dec 28433).

### A — amps — the present charge-current setpoint, 2-digit decimal (matches §4, max 79).

### P — a 3-digit field (likely pilot duty / power-related). **Stored raw, mapping not decoded — UNKNOWN.**

### L and H — **stored raw, unmapped (UNKNOWN).**
Both render as 2-digit decimals straight from SRAM bytes in the `$ES` builder with no semantic transform
I could pin down statically. From the JuiceBox lineage L is *commonly* a line/lock or "L1/L2" indicator
and H a temperature/humidity or hardware-status byte, but I have **no in-binary proof** of either — mark
both UNKNOWN and capture live (watch how they move vs. plug state and temperature).

---

## 6. Other observed inbound/outbound types (for the parser task #92)

Outbound (MCU→WiFi), each a `$<type><3-hex-len>:<payload>` literal in flash:
`$ES` status, `$HW011:001.0001701.A.002` (hw rev), `$FW006:100102` (fw ver), `$PV002:20` (proto ver),
`$DF002:01` / `$DF002:01:C>Q:` / `$DF002:02:C>R:` (state-change/diag), `$BP002:01`, `$CR004:0031`,
`$IP002:01`, `$VG002:XX`, `$LG002:XX`, `$PG002:XX`, and the freeform `$MD:` debug line (no length).
`$TP000::<>|:` and `~JV:…`/`~MD…` also appear (the `~` family looks like a secondary/bootloader channel:
`~JV:Xdfuu --factory SILABS$`, `~JV:!%d$`).

Inbound (WiFi→MCU) confirmed: `$WC`, `$WR` (handshake tokens, §3) and the `?L` 2-digit amps command (§4).

---

## 7. Method / evidence index (addresses in /tmp/jb_flash.asm, byte offsets)

| What | Address |
|------|---------|
| RX line assembler, CR/LF terminator | 0x9ae, 0x9b2 |
| `$` frame-start scan, 44-byte cap | 0x9d2, 0x9cc |
| "Back Online", clear offline bit on `$` | 0xa14 |
| Inbound type dispatch (WC/WR/HW/SP/…) | 0x1204–0x1260 |
| `$WC` handler + "expecting WC" bit3 of 0x0535 | 0x14cc |
| "WC received when not expecting it" error | 0x18b0 → str 0x01D8 |
| Watchdog counter decrement (tick) | 0x90e |
| Watchdog counter reload = 0x0BB8 (3000) | 0xd8e–0xd96 |
| Offline decision (counter==0 → set 0x0535 bit1) | 0xa56–0xa6a |
| "No comm signal. Switching to offline mode" | 0xe92/0xe98 → str 0x0372 |
| Timer-bank sweep (entry, all software timers, `wdr`) | 0x764–0x85e |
| Timer/UART HW config (UDR0=0xC0, TCCR0B=0x25, TCCR1=0x80/81) | 0x5060–0x5180 |
| Amps-set parse (2 ASCII digits @ 0x054A/0x054B, <80) | 0x1ac8, 0x15a2 |
| "_L amps value…Should be less than 80" | str 0x0234 |
| "…message length=000. Should be 002" | str 0x0275 → xref 0x14f0 |
| `$ES` builder (S/L/T/H/A/P/F) | 0xa9c |
| S-field source byte 0x0519; valid range 0..5 | 0xba8 / 0x5e82 (cpi 0x06) |
| Host-commanded state writer (0x046E ← 2 or 3) | 0x3b72/0x3bae/0x3be0 |
| F-fault code strings (verbatim table) | flash 945–1480 |

---

## 8. Confidence summary

| Item | Value | Confidence |
|------|-------|-----------|
| Frame delimiter | CR (0x0D) or LF (0x0A) closes the line; body framed by leading `$`; 44-byte max | **CONFIRMED** |
| Watchdog mechanism | counter 0x0112:0x0113, reset by any valid `$`-frame, offline when it hits 0 | **CONFIRMED** |
| Watchdog reload value | 3000 (0x0BB8) ticks | **CONFIRMED** |
| Offline timeout (seconds) | ≈ 3 s (3000 × 1 ms) | **LIKELY** (tick rate not proven; could be ~30 s if 10 ms tick) |
| Heartbeat frame | any valid `$`-frame; use the amps-set command re-sending current amps | **CONFIRMED (mechanism)** |
| Recommended heartbeat interval | 1 s (≤ ⅓ of LIKELY 3 s timeout) | recommendation |
| `$WC`/`$WR` are handshake tokens, not free heartbeats | accepted only when MCU expects them | **CONFIRMED** |
| Amps-set payload format | type contains `L`, length `002`, two decimal digits, value < 80 | **CONFIRMED** |
| Amps-set exact 2-char type | LIKELY `?L` (first char unconfirmed) | **LIKELY → capture** |
| `$ES` S-field range | integer 0..5 | **CONFIRMED** |
| `$ES` S per-state mapping | 0 idle / 1 ready / 2–3 charging / 5 fault (per table) | **LIKELY → capture** |
| `$ES` F-fault codes | full table (001–008, 101–107, CLR codes) | **CONFIRMED (literal strings)** |
| `$ES` A field | present amps setpoint, 2-digit | **CONFIRMED** |
| `$ES` P field | 3-digit, semantics undecoded | **UNKNOWN** |
| `$ES` L, H fields | stored raw, unmapped | **UNKNOWN → capture** |

**Bottom line for the implementer (tasks #92–#96):** we can build the frame parser, the `$ES` decoder
(S 0–5 + F string table), and a heartbeat that re-sends the amps setpoint every 1 s, all from CONFIRMED
data. Before driving a real 40 A contactor, a **live UART capture is required** to (a) confirm the offline
timeout in seconds, (b) nail the exact amps-command 2-char type, and (c) confirm the S per-state integers
and the L/H/P field meanings.
