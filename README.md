# FujiNet Go — Atari 5200

The Atari 5200 SuperSystem with the FujiNet cartridge in its slot: power on
into FujiNet's CONFIG, pick a game from the SD card or a network host, and
play, all in one desktop app. A member of the FujiNet Go desktop family.

| | |
|---|---|
| **Emulator** | [atari800](https://github.com/atari800/atari800), pinned and staged with small patches (`tools/atari800/patches/`), forced into 5200 mode and driven by its own platform layer. |
| **BIOS** | None needed: atari800's built-in Altirra 5200 OS runs everything. Atari's own BIOS (4-port `co19156.u8`, 2-port `co19156a.a8`) can be imported and selected in Preferences. |
| **FujiNet** | The 5200 FujiNet cartridge (`pico/atari-5200` in fujinet-firmware), emulated in-process, talking BoIP FujiBus over loopback to `libfujinet` (the firmware's RS232 PC target): BoIP port **11524**, web UI **11525**. |
| **Frontends** | GNOME (GTK4/libadwaita), KDE (Qt6 Widgets), macOS (AppKit), Windows (Win32/D3D11): display, Controllers window, Preferences, FujiNet log, debugger. |
| **Packaging** | DEB/RPM/TGZ per Linux frontend, two Flatpaks, Windows zip and NSIS installer, macOS arm64 and x86_64 bundles. |

## Using it

- **Power Cycle** (Shift+F5, **Reboot to CONFIG**) is the 5200's power
  switch: the console has no reset line, and a power cycle brings the
  FujiNet CONFIG back.
- **Open…** (or drop a file on the window) takes a 5200 cartridge image
  (`.a52`, `.bin`, `.rom`, `.car`, or a `.zip` holding one). The FujiNet
  cartridge serves it as a game and the machine power-cycles into it; the
  board (plain, 16K, 32K, Super Cart banking up to 128K) is picked from the
  image's CRC, `.car` header or size. A FujiNet application image keeps the
  cartridge's mailbox live. A real BIOS file, ZIP or folder is imported
  instead (below).
- **Preferences** are one table (`core/src/prefs.c`) every frontend builds its
  dialog from. Live settings apply at once.
- **Pause** and **Warp** (Ctrl+Shift+W) are the emulator's; the 5200's own
  PAUSE key is on the controller keypad.

### BIOS

Nothing is required. To use Atari's own BIOS, **Import BIOS…** takes any of

- one or more files,
- a ZIP (MAME's `a5200.zip` and `a5200a.zip` work),
- a folder, searched recursively; every recognised image is imported, the
  rest ignored,

recognises each image by size and CRC-32 and says what it imported and what
it ignored. A ROMs page lists each slot and whether it is imported, with its
own Import button. Imported images land in `<data dir>/roms/` and Preferences
▸ BIOS then offers **Built-in Altirra**, **Atari 5200 (4-port)** and
**Atari 5200 (2-port)**, the imported ones only. No Atari image is in any
published build (`COMPLIANCE.md`).

### Keys

Defaults; the Controllers window remaps them.

| Key | |
|---|---|
| Arrow keys | Controller 1 stick (digital, 8-way) |
| Space, Left Shift | Fire, Fire 2 (top buttons) |
| 0–9, `-` for `*`, `=` for `#` | Keypad (shared by all four controllers) |
| F4 | START |
| F3 | PAUSE |
| F5 / Shift+F5 | RESET key / Power Cycle |
| F6 | Help |
| F9 / F11 / F12 | Controllers / Fullscreen / Debugger |
| Ctrl+Shift+O, W, Q (Cmd on a Mac) | Open, Warp, Quit |

The 5200 has four controller ports. Ports 2–4 have no default keys: assign a
gamepad to each in the Controllers window (gamepads are picked up as they are
plugged in and out; the left stick or d-pad is the analog stick, the face
buttons the fire buttons), or map keys there. Dragging with the mouse in the
display moves the analog stick of the selected port.

### Debugger (F12)

Stops the machine. Run/Stop F5, Step F7, Step Over F8, Step Out Shift+F8,
Frame F9. Tabs: atari800's monitor as a console, CPU and memory, disassembly
with 5200 BIOS labels and symbol files, ANTIC and the display list, GTIA and
the palette, POKEY (including the pots and keypad), the memory map with the
FujiNet cartridge state (board, bank, mailbox registers, swap state),
breakpoints and watchpoints with conditions, and FujiNet: the BoIP FujiBus
traffic between the cartridge and FujiNet, decoded in cartridge terms.

## External FujiNet

A switch in Preferences skips the built-in FujiNet and connects the cartridge
to a FujiNet at an address:port you enter, e.g. `192.168.1.20:11524`
(`fujinet_external` and `fujinet_external_addr` in `settings.ini`). The
emulator connects to it; the other FujiNet listens for BoIP.

## Building

    cmake -B build -G Ninja
    cmake --build build
    ctest --test-dir build

Needs CMake 3.20+, a C11/C++17 compiler, SDL3, zlib, and a frontend toolkit
(GTK 4.12 + libadwaita 1.5, or Qt 6). The FujiNet runtime build also needs
Python 3 with PyYAML and Jinja2, OpenSSL and expat; `-DWITH_FUJINET=OFF`
skips it. atari800 and fujinet-firmware are fetched at their pins
(`cmake/Dependencies.cmake`). The tests need no ROM.

- **Flatpak:** `flatpak-builder --user --install build-flatpak
  build-aux/flatpak/online.fujinet.go.atari5200.gnome.yml` (or `.kde.yml`).
- **Install a released Flatpak:** download `online.fujinet.go.atari5200.gnome.flatpak`
  (or `.kde.flatpak`) from the latest release, then
  `flatpak install --user ./online.fujinet.go.atari5200.gnome.flatpak`. Flatpak
  offers to add Flathub for the runtime. Bundles don't auto-update; reinstall
  from each new release.
- **Windows:** natively in MSYS2 UCRT64, or cross-compiled with
  `cmake/toolchains/mingw-w64.cmake` and run under Wine.
- **macOS:** built on CI (`.github/workflows/macos.yml`).

**FujiNet updates itself.** A local build checks fujinet-firmware's `master`
(at most every ten minutes). When it is newer and every patch still applies,
the build moves all three pins (`cmake/Dependencies.cmake` and both flatpak
manifests) for you to commit; when a patch no longer applies it keeps the old
pin and says which. CI, flatpak and release builds use the committed pin.
`tools/fujinet/update-fujinet-firmware.sh` does it by hand.

## Licences

GPL-3.0-or-later (`LICENSE`). atari800 is GPL-2.0-or-later; the Altirra 5200
OS is Avery Lee's, freely redistributable. No Atari ROM is in any published
build (`COMPLIANCE.md`).
