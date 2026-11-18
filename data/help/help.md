This is the text of FujiNet Go Atari 5200's built-in Help (Help ▸ FujiNet Go
Atari 5200 Help, or Ctrl+Shift+H; ⌘? on a Mac). The app shows each part only
to the frontend it is about; here you see them all. Every topic below is one
page in the app.

# Getting started

FujiNet Go Atari 5200 is an Atari 5200 SuperSystem with the FujiNet cartridge
in its slot. The console is atari800 in 5200 mode. The FujiNet runs inside the
app, so FujiNet's CONFIG comes up at once and can load games from the SD card
or a network host.

1. **Start the app.** The console powers on and runs FujiNet's CONFIG. It
   needs no BIOS: atari800's built-in Altirra 5200 OS runs everything. See
   [BIOS and ROMs](#bios-and-roms).
2. **Pick a game in CONFIG.** Choose a host (the SD folder or a server on the
   network) and a game on it. FujiNet sends it to the cartridge and it
   starts. See [FujiNet](#fujinet).
3. **Or open a cartridge image of your own.** **Open…** takes a 5200
   cartridge image. See [Cartridges](#cartridges).
4. **Play.** The keys, the gamepads and the Controllers window
   (<kbd>F9</kbd>) work the four controllers. See
   [Controllers and keys](#controllers-and-keys).
5. **Change the console.** [Preferences](#preferences) picks the picture, the
   sound, the controls, the BIOS and FujiNet.

## This Help

The list on the left has a topic for each part of the app; the search box
above it finds every section that has all the words you type. A **Help**
button in Preferences, the Debugger, the Controllers window and the FujiNet
console log opens the topic for what it shows, and
<kbd>Ctrl+Shift+H</kbd> does the same for the window you are in.

<!-- only: gnome kde windows -->

In Preferences, the Debugger and the console log, <kbd>F1</kbd> does too.
(<kbd>F6</kbd> is the app's short list of keys, not this Help.)

<!-- end -->

The same text is on GitHub, as
[data/help/help.md](https://github.com/FujiNetWIFI/fujinet-go-atari5200-desktop/blob/main/data/help/help.md).

<!-- only: macos -->

On a Mac, the Help menu's search field also finds Help topics.

<!-- end -->

<!-- context: main -->
# The window

The console's picture fills the window. The menus have:

- **Open…**, **Power Cycle** (<kbd>Shift+F5</kbd>), **Reboot to CONFIG**,
  **RESET Key** (<kbd>F5</kbd>) and **Warp Speed**.
- **Import BIOS**, **Import to SD Card…**, **Open SD Folder** and **Open Media
  Folder**.
- **Controllers** (<kbd>F9</kbd>) and **Debugger** (<kbd>F12</kbd>).
- **TV Aspect**, **Smooth Scaling** and **Fullscreen** (<kbd>F11</kbd>).
- **FujiNet Web UI**, **Console Log** and **Restart FujiNet**.
- **Preferences**, **Help**, **Keys** (<kbd>F6</kbd>), **About** and **Quit**.

<!-- only: gnome -->

On GNOME, everything is in the main menu (the button at the right of the
header bar).

<!-- end -->

<!-- only: kde windows -->

The menu bar has Machine, View, FujiNet and Help.

<!-- end -->

<!-- only: macos -->

On a Mac, Preferences (called Settings) and Quit are in the app menu, and
the rest are in the Machine, View, FujiNet and Help menus.

<!-- end -->

## Power and reset

- **Power Cycle** (<kbd>Shift+F5</kbd>) is the console's power switch. The 5200
  has no reset line, so a power cycle is how you start over, and it brings
  FujiNet's CONFIG back.
- **Reboot to CONFIG** ejects an opened cartridge and power-cycles.
- **RESET Key** (<kbd>F5</kbd>) presses the keypad's RESET key, which the
  program reads like any other key.
- **Pause** is the 5200's own PAUSE key on the controller keypad
  (<kbd>F3</kbd>). **Warp Speed** runs the console as fast as the host allows.

## The picture

**TV Aspect** shows the picture 4:3 as a television does, or with square
pixels. **Smooth Scaling** blends the pixels when the picture is stretched.
Preferences ▸ Video picks how much of the picture shows, and its colours.

# Cartridges

**Open…** (<kbd>Ctrl+Shift+O</kbd>) takes a 5200 cartridge image: an `.a52`,
`.bin`, `.rom` or `.car`, or a `.zip` that holds one. Dropping the file on the
window does the same. The FujiNet cartridge serves it as a game and the
console power-cycles into it. The board (plain, 16K, 32K, or Super Cart
banking up to 128K) is picked from the image's checksum, its `.car` header or
its size. An image that is a FujiNet application keeps the cartridge's mailbox
live. A real BIOS file, zip or folder is imported instead (see
[BIOS and ROMs](#bios-and-roms)). If the file cannot be used, a message says
why and what was running goes on.

<!-- context: import-sd -->
## Import to SD Card

**Import to SD Card…** copies a cartridge image into FujiNet's SD folder, so
CONFIG can list it and boot it through the cartridge. **Open SD Folder** and
**Open Media Folder** show those folders in the file manager.

# BIOS and ROMs

Nothing is required: atari800's built-in Altirra 5200 OS runs everything. To
use Atari's own BIOS, import it. The BIOS is copyrighted, so none comes with
the app.

| ROM | Is |
|---|---|
| Atari 5200 BIOS (4-port) | `co19156.u8`, the original four-port console |
| Atari 5200 BIOS (2-port) | `co19156a.a8`, the two-port console |

**Import BIOS** (in the Machine menu, and on the BIOS page of Preferences)
takes any of:

- one or more files,
- a zip (MAME's `a5200.zip` and `a5200a.zip` work),
- a folder, searched recursively: every recognised image is imported, the
  rest ignored.

Each image is recognised by its size and checksum, whatever it is called. The
message says what was imported and what was ignored. Imported images are
copied into the app's `roms` folder (see [Files and folders](#files-and-folders)),
so the originals can go. Preferences ▸ BIOS then offers **Built-in Altirra**,
**Atari 5200 (4-port)** and **Atari 5200 (2-port)**, the imported ones only.

# Controllers and keys

The 5200 has four controller ports. Each controller has an analog stick, two
fire buttons (top buttons) and a keypad (0 to 9, `*` and `#`, START, PAUSE and
RESET). With the 2-port BIOS (Preferences ▸ BIOS) the console has two
controllers, and the app shows two. The keypad and the second fire button are
shared by all the controllers: atari800 scans one keypad and one FIRE 2, so a
key held on any controller is held on all of them.

## Default keys

Keys go by where they are on the keyboard, whatever your layout.

| Does | Key |
|---|---|
| Controller 1 stick | <kbd>Up</kbd>, <kbd>Down</kbd>, <kbd>Left</kbd>, <kbd>Right</kbd> |
| Fire | <kbd>Space</kbd> |
| Fire 2 | <kbd>Left Shift</kbd> |
| Keypad 0 to 9 | The digits <kbd>0</kbd> to <kbd>9</kbd> |
| Keypad `*` | <kbd>-</kbd> |
| Keypad `#` | <kbd>=</kbd> |
| START | <kbd>F4</kbd> |
| PAUSE | <kbd>F3</kbd> |
| RESET | <kbd>F5</kbd> |

Ports 2 to 4 have no default keys: give each a gamepad in the Controllers
window, or map keys there. START, PAUSE, RESET and the keypad are common to
every port, so they have one key each.

The app keeps these keys for itself:

| Shortcut | Does |
|---|---|
| <kbd>Shift+F5</kbd> | Power Cycle |
| <kbd>F6</kbd> | The app's short list of keys |
| <kbd>F9</kbd> | Controllers |
| <kbd>F11</kbd> | Fullscreen |
| <kbd>F12</kbd> | Debugger |
| <kbd>Ctrl+Shift+O</kbd> | Open |
| <kbd>Ctrl+Shift+Q</kbd> | Quit |
| <kbd>Ctrl+Shift+H</kbd> | Help |

<!-- only: macos -->

On a Mac the shortcuts use ⌘ instead: ⌘O and ⌘Q.

<!-- end -->

<!-- only: linux -->

Key combinations with the Super (Windows) key are the desktop's.

<!-- end -->

## Gamepads

Gamepads are picked up as they are plugged in, and can come and go at any
time; a message shows when one comes or goes. The left stick or the D-pad is
the analog stick, and the face buttons are the fire buttons. With **Gamepad
sticks are analog** off (Preferences ▸ Input), the left stick is an
eight-way switch.

Each pad plays one controller port. **Automatic** gives the first pad port 1,
the next port 2, and so on; choose **Port 1** to **Port 4** to fix it, in the
Controllers window or, where Preferences has the rows, in Preferences ▸ Input.
A pad that is fixed to a port takes it: a pad left on *Automatic* then plays
nothing if there is no port left for it, and so does a second pad fixed to the
same port. With the 2-port BIOS only Port 1 and Port 2 can be chosen, and a
choice past them falls back to *Automatic*.

A choice is remembered between runs (setting `pad_assign`), by the pad's id
and name, so a pad that is unplugged and plugged in again gets its port back.
Unplugging a pad lets go of everything it held. A gamepad's other buttons can
be given the keypad, START, PAUSE, RESET and fire in the Controllers window.

## The mouse

Dragging with the mouse in the picture moves the analog stick of one
controller: the one chosen by **Mouse in the picture drives** in the
Controllers window (setting `mouse_port`, Controller 1 to 4). Let go and the
stick centres. A drag never cancels a gamepad or a key that holds the same
stick: the strongest deflection wins.

## Changing the controls

The controls are changed in the Controllers window (<kbd>F9</kbd>): click
**Map**, then click a control in the picture (a stick, a fire button, a
keypad key or START, PAUSE, RESET), then press the key or the gamepad button
to give it. There are 42 controls in all: the four ports' stick directions,
FIRE and FIRE 2 (24), the twelve keypad keys and START, PAUSE and RESET (15),
and the system actions Power cycle, Pause (stop in the Debugger) and Warp
(3). The **list** next to the buttons names every one of them.

- **Clear** takes the picked control's key and gamepad button away.
- **Defaults** puts every control back.
- **Show bindings** writes the key bound to each control on it.
- <kbd>Escape</kbd> backs out of a pick; <kbd>F9</kbd>, <kbd>F11</kbd>,
  <kbd>F12</kbd> and keys with <kbd>Ctrl</kbd> cannot be given to a control.

A key does one thing, so a key given to a new control is taken from the old
one, and the window says so. The keypad, START, PAUSE, RESET and FIRE 2 are
common to all the controllers, so a key or button given to one is that
control on every port. The map is kept in the settings, and only what differs
from the defaults is written.

<!-- context: controllers -->
# Controllers window

The Controllers window (<kbd>F9</kbd>) draws the Atari 5200's controllers like
the real thing: the CX52 with its stick in a dish, the red fire buttons on
both sides, START, PAUSE and RESET, and the twelve-key pad. There are four,
side by side, or two with the 2-port BIOS. It floats on its own at a fixed
size, hidden until you ask for it. A note under the controllers says that the
keypad and the second fire button are shared by all of them: a key held on one
is held on all.

Whatever the console sees held lights up in orange, whoever holds it: the
keyboard, a gamepad or the mouse, so the window is also an input tester for a
new gamepad.

| With the mouse | Does |
|---|---|
| Press a key (keypad, START...) | Holds it for as long as the button is down |
| Press a fire button | Holds it (the upper pair is FIRE, the lower pair FIRE 2) |
| Drag a stick | Moves it towards the pointer; let go to centre |
| Scroll over a stick | Nudges it |

Switching to another window lets go of everything the mouse holds. Keys typed
in the Controllers window go to the console, except while **Map** waits for a
key.

Under the picture are a row for each gamepad plugged in (*Automatic* or
*Port 1* to *Port 4*, see [Gamepads](#gamepads)), **Mouse in the picture
drives** (see [The mouse](#the-mouse)), and the **Map**, **Clear**,
**Defaults** and **Show bindings** buttons with the list of all the controls
(see [Changing the controls](#changing-the-controls)). The **Show bindings**
check box is remembered (setting `controllers_show_bindings`). A **Help**
button opens this topic. The window can also be opened at start with the
environment variable `A52_OPEN_CONTROLLERS` set.

# FujiNet

The FujiNet is FujiNet's own firmware, running inside the app, and the
cartridge in the slot is its connection to the console. On the real console
the FujiNet cartridge is a board behind the cartridge door; here it is
emulated, and talks to FujiNet over a local network socket (BoIP, FujiBus over
TCP).

## CONFIG

CONFIG is FujiNet's program for choosing what to run:

- **Hosts** are where games come from: the SD folder, and servers on the
  network.
- Pick a host and a game on it. FujiNet sends the image to the cartridge,
  which swaps it in and power-cycles the console on it.
- **Reboot to CONFIG** (Machine menu) brings CONFIG back.

## The FujiNet menu

- **FujiNet Web UI** opens FujiNet's settings page in your browser, at
  `http://127.0.0.1:11525/`. Only this computer can reach it.
- **Console Log** shows what FujiNet prints. See
  [The console log](#the-console-log).
- **Restart FujiNet** restarts the built-in FujiNet.

<!-- context: fujilog -->
## The console log

The FujiNet Console Log window shows FujiNet's own messages as they come,
following the end while you are at the bottom. It is also kept in
`fujinet-console.log` in the app's `fujinet` folder.

## Turning it off

Preferences ▸ FujiNet ▸ **FujiNet** turns the built-in FujiNet off.

## An external FujiNet

Preferences ▸ FujiNet ▸ **Use external FujiNet** skips the built-in FujiNet
and connects the cartridge to a FujiNet at the address you give, for example
`192.168.1.20:11524`. The emulator connects to it, and the other FujiNet
listens for BoIP. FujiNet Web UI and Console Log belong to the built-in
FujiNet, so they say the FujiNet is external; use the other instance's own.
The Debugger's FujiNet tab still shows the traffic.

## Behind the scenes

- The cartridge and FujiNet talk over TCP port 11524 on 127.0.0.1 (the
  BoIP port), and the web UI is on port 11525. Nothing from outside can
  reach either.
- FujiNet's settings are `fnconfig.ini` in the app's `fujinet` folder. It is
  written once and then left for FujiNet and you.

# Preferences

Preferences (<kbd>Ctrl+,</kbd>) set up the console. Each setting says when it
takes effect: at once, with a power cycle (done when Preferences closes), or
by restarting the session. The settings are one file, shared by the GNOME and
KDE apps on the computer.

<!-- only: macos -->

On a Mac this window is called **Settings** (in the app menu), and its
pages are tabs.

<!-- end -->

The pages are Video, Audio, Input, BIOS and FujiNet. The BIOS page also lists
each BIOS slot, *Imported* or *Not imported*, with the Import buttons.

<!-- context: prefs/Video -->
## Video settings

The picture. The TV system applies with a power cycle; the rest at once.


### The console

- **TV system**: *NTSC* or *PAL*. The 5200 is an NTSC console; PAL changes atari800's timing and palette only (setting `tv`)

### Colour

- **Palette**: the colour palette: *Standard*, *Deep black* or *Vibrant* (setting `palette_preset`)
- **Saturation**: colour saturation, -100 to 100 (setting `saturation`)
- **Contrast**: contrast, -100 to 100 (setting `contrast`)
- **Brightness**: brightness, -100 to 100 (setting `brightness`)
- **Gamma**: gamma, -100 to 100 (setting `gamma`)
- **Tint**: tint (hue), -100 to 100 (setting `hue`)
- **External palette (.act)**: an external palette, an `.act` file, in place of the preset (setting `palette_file`)

### Picture

- **Visible area**: how much of the picture shows: *Narrow (320 × 200)*, *TV (336 × 224/240)* or *Overscan (384 × 240)* (setting `area`)
- **Aspect**: *TV (4:3)* or *Square pixels* (setting `aspect`)
- **Smooth scaling**: blends the pixels when the picture is stretched; off keeps them sharp (setting `smooth`)

<!-- context: prefs/Audio -->
## Audio settings

The sound. The volume applies at once; the POKEY option with a power cycle.


### Sound

- **Volume**: the output level, 0 to 100 (setting `volume`)
- **High-fidelity POKEY**: the high-fidelity POKEY sound (setting `mz_pokey`)

<!-- context: prefs/Input -->
## Input settings

How a gamepad drives the controllers. Applies at once, except Enable
gamepads, which restarts the session.


### Controllers

- **Enable gamepads**: pick up USB and Bluetooth gamepads at all; each is
  assigned to a controller in the Controllers window (setting `enable_gamepad`)
- **Gamepad sticks are analog**: the 5200's controllers are analog; off makes a gamepad's left stick an eight-way switch (setting `analog_stick`)
- **Autofire**: *Off*, *While fire is held* or *Always* (setting `autofire`)

### Gamepad ports

<!-- only: gnome kde macos -->

A row for each gamepad plugged in, with the port it plays: *Automatic* or
*Port 1* to *Port 4*, applied at once (see [Gamepads](#gamepads)). The rows
follow gamepads plugged in and out while Preferences is open.

<!-- end -->

<!-- only: windows -->

Settings has no per-pad rows: assign each gamepad a port in the Controllers
window (see [Gamepads](#gamepads)).

<!-- end -->

### Controls buttons

- **Reset Controls**: puts the default keys and gamepad buttons back, and every
  gamepad on *Automatic*.
- **Open Controllers**: opens the Controllers window (see
  [Controllers window](#controllers-window)).

<!-- context: prefs/BIOS -->
## BIOS settings

Which console BIOS runs. Applies with a power cycle.


### Console BIOS

- **BIOS**: which BIOS runs: *Built-in Altirra*, or an imported *Atari 5200 (4-port)* or *Atari 5200 (2-port)*. The built-in Altirra OS runs everything (setting `bios`)

<!-- context: prefs/FujiNet -->
## FujiNet settings

FujiNet and the host's audio. These restart the session. (Gamepads are on the
Input page.)


### The built-in FujiNet

- **FujiNet**: the built-in FujiNet, behind the cartridge (setting `enable_fujinet`)

### External FujiNet

- **Use external FujiNet**: skips the built-in FujiNet and dials another instance (setting `fujinet_external`)
- **Address:port**: that instance's BoIP listener, for example `192.168.1.20:11524`. The cartridge dials it (setting `fujinet_external_addr`)

### Host

- **Sound output**: opens the system audio device (setting `enable_audio`)

## The settings file

The settings file is `settings.ini` (see [Files and folders](#files-and-folders)).
Each setting above names its key.

The Controllers window keeps its own settings here too: `bindings` (the keys
and gamepad buttons that differ from the defaults), `pad_assign` (the port each
gamepad was given, by its id and name), `mouse_port` (the controller the mouse
drives), and `controllers_show_bindings` (the check box under the picture).

# Debugger

The Debugger (<kbd>F12</kbd>) stops the console and shows what it is doing.
It has nine tabs; closing it lets the console go on as it was.

| Button | Does |
|---|---|
| Stop or Run (<kbd>F5</kbd>) | Stops before the next instruction, or runs on |
| Step (<kbd>F7</kbd>) | One instruction |
| Step Over (<kbd>F8</kbd>) | Over a JSR, to the instruction after it |
| Step Out (<kbd>Shift+F8</kbd>) | On until the routine returns |
| Frame (<kbd>F9</kbd>) | On to the next frame |

<!-- context: debugger/console -->
## Console

atari800's monitor, as a console. Type `HELP` in it for its commands.

<!-- context: debugger/cpu -->
## CPU and memory

The 6502's registers and flags, an editable hex view of memory, and the
history of the last instructions. Click a value to edit it while stopped.

<!-- context: debugger/disassembly -->
## Disassembly

The 6502 disassembled at the PC, with the 5200 BIOS's labels, the opcode bytes
and the breakpoints in the margin. Click the left edge of a line for a
breakpoint. Symbol files can be loaded.

<!-- context: debugger/antic -->
## ANTIC

ANTIC's registers, the display list decoded line by line, and where the beam
is.

<!-- context: debugger/gtia -->
## GTIA

GTIA's registers, the colour palette as swatches, and the collisions.

<!-- context: debugger/pokey -->
## POKEY

POKEY's registers, including the pots and the keypad.

<!-- context: debugger/map -->
## Memory map

The memory map, with the FujiNet cartridge's state: its board, the bank, the
mailbox registers and the swap state.

<!-- context: debugger/breakpoints -->
## Breakpoints

The list of breakpoints and watchpoints with their conditions. **Add**,
**Remove**, and the check box to turn one off.

<!-- context: debugger/fujinet -->
## FujiNet packets

The BoIP FujiBus traffic between the cartridge and FujiNet, decoded in the
cartridge's terms, from the moment the app starts. **Clear** empties the list.

# Files and folders

The app keeps its settings in one file, and everything else in its data
folder:

<!-- only: linux -->

On Linux, the settings are `~/.config/fujinet-go-atari5200/settings.ini` and
the data is in `~/.local/share/fujinet-go-atari5200` (or under
`$XDG_CONFIG_HOME` and `$XDG_DATA_HOME`). The GNOME and KDE apps share them.
As a Flatpak, they are under `~/.var/app/`, in the app's own folder.

<!-- end -->

<!-- only: windows -->

On Windows, the settings are `%APPDATA%\fujinet-go-atari5200\settings.ini` and
the data is in `%LOCALAPPDATA%\fujinet-go-atari5200`.

<!-- end -->

<!-- only: macos -->

On a Mac, the settings are `~/.config/fujinet-go-atari5200/settings.ini` and
the data is in `~/.local/share/fujinet-go-atari5200`.

<!-- end -->

In the data folder:

| Folder | Has |
|---|---|
| `roms` | the imported BIOS images |
| `media` | cartridge images opened or dropped on the window |
| `fujinet` | `fnconfig.ini`, the SD folder (`SD`), and the console log |

# Troubleshooting

- **A cartridge was refused**: the message says why: too big, empty, or not a
  5200 cartridge image. A zip must hold exactly one image.
- **The game does not start**: the board is picked from the image; a
  mis-sized or unknown image may need another file.
- **A BIOS was not taken**: BIOS images are recognised by size and checksum;
  get them from MAME's `a5200.zip` and `a5200a.zip`.
- **Keys don't reach the console**: a dialog or another window is in front,
  or the key is one the app keeps (see
  [Controllers and keys](#controllers-and-keys)).
- **A gamepad does nothing**: it may be on a port with no controller, or its
  buttons given other jobs in the Controllers window.
- **No sound**: the audio device could not be opened (Preferences ▸ FujiNet ▸
  Sound output), or the volume is 0.
- **A port is taken**: another program is using 11524 or 11525. Close the
  other FujiNet, or choose **Use external FujiNet**.

# About and licences

FujiNet Go Atari 5200 is free software under the GNU General Public License,
version 3 or later. Copyright © 2026 Thomas Cherryhomes.

- **atari800** (GPL-2.0-or-later) is the emulator.
- **The Altirra 5200 OS** (Avery Lee, freely redistributable) is built in.
- **The FujiNet firmware** (GPL-3.0-or-later) runs inside the app, and the
  cartridge's own sources are the firmware's.
- No Atari ROM is part of the app; you import your own.

[fujinet.online](https://fujinet.online/) ·
[Report an issue](https://github.com/FujiNetWIFI/fujinet-go-atari5200-desktop/issues)
