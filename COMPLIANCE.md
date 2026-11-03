# Compliance

## What ships

| Component | Origin | Licence | How it enters the build |
|---|---|---|---|
| This application | this repository | GPL-3.0-or-later | — |
| **atari800** (the emulator: CPU, ANTIC, GTIA, POKEY, 5200 cartridges, monitor) | [atari800/atari800](https://github.com/atari800/atari800), pinned in `cmake/Dependencies.cmake`, plus `tools/atari800/patches/` | GPL-2.0-or-later | `src/` staged and patched by `cmake/StageAtari800.cmake`, compiled with `core/atari800/config.h`, forced into 5200 mode |
| **Altirra 5200 OS** (built-in BIOS replacement) | atari800's `src/roms/` (built from its `emuos/`) | permissive (notice-preserving), © Avery Lee | compiled into the core; the default BIOS |
| **FujiNet firmware** (the 5200 cartridge's own sources, and `libfujinet`) | [FujiNetWIFI/fujinet-firmware](https://github.com/FujiNetWIFI/fujinet-firmware), `pico/atari-5200` sources staged verbatim, RS232 PC target built as `libfujinet`, plus `tools/fujinet/patches/` | GPL-3.0-or-later | cartridge sources staged at configure time; `libfujinet` built as a shared library by `tools/fujinet/build-fujinet-desktop.sh`, `dlopen`ed at run time |
| SDL3 | libsdl-org | Zlib | audio and gamepads; system package on Linux, static in releases |
| zlib | zlib.net | Zlib | zipped BIOS and cartridge imports |
| mbedTLS 3.6.x | Mbed-TLS | Apache-2.0 | for `libfujinet` |
| OpenSSL 3, expat | openssl.org, libexpat | Apache-2.0, MIT | for `libfujinet` (its bundled libssh and libsmb2, its HTTP parser); system libraries on Linux and macOS, linked into `fujinet.dll` on Windows |

The combined work is distributed under GPL-3.0.

## What is changed in atari800

Each patch in `tools/atari800/patches/` is small and named for its purpose;
none changes emulation, which the `ab_*` tests check (patched and unpatched
cores, run deterministically, produce identical frames and audio).

## The Atari 5200 BIOS — none ships

The Atari 5200 BIOS (`co19156.u8`, the 4-port console, CRC-32 4248d3e3, and
`co19156a.a8`, the 2-port revision, CRC-32 c2ba2613) and Atari's cartridge
games are copyrighted. **This project distributes none of them, and none is
needed:** the built-in Altirra 5200 OS runs the machine, and FujiNet's own
CONFIG is staged from the firmware.

- **Import BIOS…** (files, a ZIP, or a folder) copies a user's own image into
  the data directory, recognised by size and CRC-32; Preferences then offers
  it. Real BIOS images are only ever imported by the user, never embedded.
- **`WITH_ATARI800_ROMS`** (default OFF, and OFF for every published
  artifact) lets a developer's build embed recognised images from
  `tools/roms/`. The `no_embedded_roms_*` tests check every shipped binary,
  and each release's assets are unpacked and scanned the same way
  (`core/tests/no_embedded_roms.py`).
- The tests need no ROM: they build their cartridges themselves.

## Icon and name

The icon is the Atari 5200 controller (stick, keypad, START and PAUSE),
drawn for this project from the console photo, in the silver of the brushed
band on the black case (`tools/icons/draw-mark.py`, `tools/icons/make-icons.py`). "Atari" and "5200"
are trademarks of Atari Interactive, used to name the machine emulated; the
project is not affiliated with or endorsed by Atari.
