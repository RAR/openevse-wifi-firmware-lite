#!/usr/bin/env python3
"""Generate a dual-bank OTA UF2 for the EFM32GG11 (wgm160p-juicebox-40) by driving
uf2tool.UF2Writer directly, bypassing the missing ltchiptool EFM32GG11 SocInterface.

UF2Writer.write() only reads soc.ota_type (DUAL) and soc.ota_supports_format_1 from
the SoC object; everything else (family id 0xEF326611, board flash regions ota1/ota2,
the diff32 binpatch) comes from families.json / the board JSON / the input bins. So a
2-field stub is enough. Output: a real dual-bank UF2 the on-device lt_ota accepts.
"""
import sys
# import ltchiptool fully first to avoid the uf2tool<->ltchiptool circular import
import ltchiptool
from ltchiptool import SocInterface
from ltchiptool.models import OTAType, BoardParamType
from uf2tool.writer import UF2Writer
from uf2tool.models import Image

BUILD = "/home/rar/oevse/openevse-wifi-firmware-lite/.pio/build/openevse_lite"
OUT   = "/home/rar/oevse/openevse-wifi-firmware-lite/.pio/build/openevse_lite/firmware_ota.uf2"

class _EFM32Stub:
    ota_type = OTAType.DUAL
    ota_supports_format_1 = True

_orig_get = SocInterface.get
def _patched_get(family):
    code = getattr(family, "code", "") or ""
    name = getattr(family, "name", "") or ""
    if "efm32gg11" in code or "efm32gg11" in name:
        return _EFM32Stub()
    return _orig_get(family)
SocInterface.get = staticmethod(_patched_get)

board  = BoardParamType().convert("wgm160p-juicebox-40", None, None)
family = board.family
print(f"family: {family.name} code={family.code} id=0x{family.id:08X} is_chip={family.is_chip}")

with open(OUT, "wb") as f:
    w = UF2Writer(f, family, legacy=True)
    w.set_board(board)
    w.set_version("1.12.1")
    w.set_firmware("openevse-wifi-firmware-lite:26.06.23")
    w.set_date(1782200000)
    w.write([Image(f"{BUILD}/firmware_a.bin,{BUILD}/firmware_b.bin=device:ota1,ota2")])

import os
print(f"wrote {OUT} ({os.path.getsize(OUT)} bytes)")
