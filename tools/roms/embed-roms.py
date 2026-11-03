#!/usr/bin/env python3
"""Generate roms_embedded.c for a WITH_A5200_ROMS developer build.

    embed-roms.py <out.c> <dir>     embed the recognised images in <dir>
    embed-roms.py --none <out.c>    an empty table (every published build)

An image is recognised by size and CRC-32 (the two Atari 5200 BIOS revisions)
or by its name: <slot key>.rom, the names core/src/roms.c stores imports
under. Never ship a build made with images.
"""
import sys
import zlib
from pathlib import Path

KNOWN = {  # (size, crc32): slot key -- core/src/roms.c s_slots[]
    (0x800, 0x4248d3e3): "bios_4port",     # MAME co19156.u8
    (0x800, 0xc2ba2613): "bios_2port",     # MAME co19156a.a8
}
NAMED = {"bios_4port", "bios_2port"}


def main():
    args = sys.argv[1:]
    none = args and args[0] == "--none"
    if none:
        args = args[1:]
    out = Path(args[0])
    found = {}
    if not none:
        for f in sorted(Path(args[1]).glob("*")):
            if not f.is_file() or f.suffix == ".py":
                continue
            data = f.read_bytes()
            key = KNOWN.get((len(data), zlib.crc32(data) & 0xffffffff))
            if not key and f.stem in NAMED:
                key = f.stem
            if key:
                found[key] = data
    lines = ['#include "roms_embedded.h"', ""]
    for i, (key, data) in enumerate(sorted(found.items())):
        lines.append(f"static const uint8_t rom{i}[] = {{")
        for j in range(0, len(data), 16):
            lines.append("    " + ",".join(f"0x{b:02x}" for b in data[j:j + 16]) + ",")
        lines.append("};")
    lines.append("const a52_embedded_rom a52_embedded_roms[] = {")
    for i, (key, data) in enumerate(sorted(found.items())):
        lines.append(f'    {{"{key}", rom{i}, {len(data)}u}},')
    lines.append('    {0, 0, 0}')
    lines.append("};")
    lines.append(f"const unsigned a52_embedded_rom_count = {len(found)};")
    out.write_text("\n".join(lines) + "\n")
    print(f"embed-roms: {len(found)} image(s) -> {out}")


if __name__ == "__main__":
    main()
