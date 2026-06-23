# Recovering a virgin/blank EFM32GG11 (WGM160P) so it boots lite firmware

## What was wrong (diagnosed 2026-06-22 on the bench)

The lite firmware flashed + verified perfectly (bootloader @ 0x0, app @ 0x8000),
but the board produced **no UART output and no Wi-Fi AP**. Halting via J-Link
showed the core in a **HardFault loop**:

- `IPSR = 3` (HardFault), stuck spinning in the bootloader's fault handler (0x76C)
- `CFSR = 0x00010000` → UsageFault **UNDEFINSTR** (undefined instruction)
- `HFSR = 0x40000000` → FORCED (escalated to HardFault)
- Stacked fault **PC = 0x0FE10000**, all regs 0 → faulted at the reset entry
- Reset-and-halt latched **PC = 0x0FE10000** before executing anything

`0x0FE10000` is the GG11 **bootloader region**, and on this part it is **empty
(all 0xFF)**. The config page was also fully erased:

- `0x0FE04000` (lock-bits page) = all `0xFFFFFFFF`
- `0x0FE041E8` (**CLW0**, boot-source config word) = `0xFFFFFFFF`

### Root cause

EFM32GG11 boot source is selected by **CLW0 bit 1 (BOOTLOADER_ENABLE)**:

| CLW0[1] | Boot from | Notes |
|--------|-----------|-------|
| `1` (erased default) | `0x0FE10000` bootloader region | **our chip** → region empty → UNDEFINSTR → HardFault |
| `0` | `0x00000000` main flash | bootloader bypassed → **our lite bootloader runs** |

A normally-provisioned JuiceBox WGM ships with CLW0[1]=0 (and/or a valid Gecko
bootloader in the region), which is why `lite_flash.sh` "just works" on real
units — it never touches this page. **This module is a bare/erased GG11**, so we
have to set the boot source ourselves.

## The fix

Clear CLW0[1]: program `0xFFFFFFFF → 0xFFFFFFFD` at `0x0FE041E8`. That is a single
1→0 bit change, so **no page erase is needed** — just one MSC word-write. The
lite image is already on the part, so a reset after this boots straight into it.

All addresses/values verified against the EFM32GG11B device headers, Black Magic
`efm32.c`, and the GG11 reference manual; MSC base for GG11 is **0x40000000**
(not the generic Series-1 `0x400E0000`).

## Morning procedure (push-button)

USB note: only one tool can own the J-Link at a time — make sure no other
`JLinkExe`/`openocd` is holding it (`ps aux | grep -i jlink`).

### Step 1 (optional but recommended) — confirm the target value on a donor
Cable the J-Link to a **known-good JuiceBox WGM** and run:
```
JLinkExe -ExitOnError 0 -CommanderScript scripts/gg11_read_clw0.jlink
```
The 3rd word (offset 0x1E8 → `0x0FE041E8`) is CLW0. Expect **`FFFFFFFD`**
(bit1 clear). If the donor shows a *different* CLW0, use that exact value as
`MSC_WDATA` in `gg11_fix_clw0_boot.jlink` (line `w4 0x40000018, ...`).
Also check the last word (DLW @0x1FC): if it's not `FFFFFFFF` the donor is
debug-locked — ignore that, don't copy it.

### Step 2 — apply the fix to the blank GG11
Cable back to our board and run:
```
JLinkExe -ExitOnError 0 -CommanderScript scripts/gg11_fix_clw0_boot.jlink
```
**Success criteria in the output:**
- `POST: CLW0` reads **`FFFFFFFD`**
- `reset-halt ... read PC` shows **PC ≠ 0x0FE10000** (should be ~`0x0000072C`,
  our bootloader's reset handler)

### Step 3 — confirm it's alive
The script ends with `g` (go), so the board is running lite. Since flash was
fully erased there are no stored Wi-Fi creds → it should come up in **softAP
provisioning mode**. Watch for the `OpenEVSE`/JuiceBox AP; `/scan` + `/connect`
onto the LAN; then `curl http://<ip>/status`.

If no AP after ~30 s, re-halt and re-read PC/CFSR — if it's faulting again,
capture the new fault PC and we diagnose from there.

## If something goes sideways

This is **recoverable**, not a brick. An AAP device-erase
(`JLinkExe` → `unlock EFM32Gxxx`, or a full erase) returns CLW0 to `0xFFFFFFFF`;
then re-flash with `lite_flash.sh` (or J-Link `loadbin`) and re-apply this fix.
We deliberately do **not** write DLW/ULW/MLW, so debug access stays open
throughout.

## Files
- `gg11_fix_clw0_boot.jlink` — the fix (MSC-programs CLW0, verifies, resets)
- `gg11_read_clw0.jlink` — read-only lock-word dump (donor cross-check / verify)
