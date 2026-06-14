#!/usr/bin/env bash
# Custom uploader for [env:openevse_lite] (WGM160P / EFM32GG11 via J-Link/SWD).
#
# The WGM160P flash layout is: first-stage bootloader @ 0x00000000 (32 KB) + app @ 0x00008000.
# We flash the two flat binaries the LibreTiny build produces (bootloader.bin + firmware.bin)
# at those offsets.
#
# DO NOT `openocd program raw_firmware.elf` — that ELF has a LOAD segment based at p_paddr=0x0
# whose first 0x8000 bytes are the ELF file header + padding (real .text is at 0x8000). openocd
# programs by physical address, so it would paint "\x7fELF..." over the bootloader at 0x0; at
# reset the core then reads a garbage initial stack pointer -> BusFault while stacking -> double
# fault -> lockup. Flashing the two .bin images at their correct offsets is the safe path.
#
# OTA METADATA: the bootloader is dual-bank (A @ 0x008000, B @ 0x100000) and, when valid
# ping-pong OTA metadata exists (pages 0x1F8000/0x1F9000), it boots whatever bank that metadata
# selects — IGNORING a fresh bank-A image written here. After any OTA round leaves metadata
# pointing at bank B, a bench reflash would silently keep booting the stale bank-B image (symptom:
# no LEDs / board never serves, VTOR=0x00100000). So we ERASE both metadata pages on every bench
# flash; the bootloader then falls through its "no/invalid metadata -> boot bank A if sane" path
# and boots exactly what we just wrote. (Layout mirrors lt_ota_meta.h in the LibreTiny fork.)
#
# Invoked by PlatformIO via `upload_command` with $BUILD_DIR as $1.
set -euo pipefail

BUILD_DIR="${1:-}"
# Resolve the project dir from this script's location (scripts/ lives at the project root) so
# openocd finds target/efm32.cfg regardless of the caller's cwd.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_DIR"

if [ -z "$BUILD_DIR" ]; then
  BUILD_DIR=".pio/build/openevse_lite"
fi

# Pick an openocd that can actually talk to the J-Link OB. PlatformIO prepends its bundled
# tool-openocd (xPack 0.11) to PATH, but on this host that build reports "No J-Link device found";
# the system openocd (0.12) works. Prefer an explicit system path; override with $OPENOCD.
OPENOCD="${OPENOCD:-}"
if [ -z "$OPENOCD" ]; then
  for cand in /usr/bin/openocd /usr/local/bin/openocd; do
    if [ -x "$cand" ]; then OPENOCD="$cand"; break; fi
  done
  [ -z "$OPENOCD" ] && OPENOCD="openocd"
fi

BOOT="$BUILD_DIR/bootloader.bin"
APP="$BUILD_DIR/firmware.bin"

for f in "$BOOT" "$APP"; do
  if [ ! -f "$f" ]; then
    echo "lite_flash: missing $f (run a build first)" >&2
    exit 1
  fi
done

echo "lite_flash: bootloader.bin -> 0x00000000, firmware.bin -> 0x00008000 (J-Link/SWD via $OPENOCD)"
exec "$OPENOCD" \
  -c "adapter driver jlink" \
  -c "transport select swd" \
  -f target/efm32.cfg \
  -c "init" \
  -c "reset halt" \
  -c "flash erase_address unlock 0x001F8000 0x2000" \
  -c "program $BOOT 0x00000000 verify" \
  -c "program $APP 0x00008000 verify" \
  -c "reset run" \
  -c "exit"
