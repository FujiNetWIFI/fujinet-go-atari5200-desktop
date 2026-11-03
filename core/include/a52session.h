/*
 * a52session -- the toolkit-agnostic desktop session for FujiNet Go Atari 5200.
 *
 * Owns the emulator (atari800 forced into 5200 mode, on a thread of its own,
 * core/atari800), the FujiNet CARTRIDGE (core/cart: a C transposition of the
 * firmware's own MAME device, over the cartridge firmware's sources), the SDL
 * audio and gamepad backends, the in-process FujiNet runtime, the settings
 * store, the key/pad bindings, imported BIOS images and the media layout.
 * Frontends (GTK4, Qt6, AppKit, Win32) do windowing, painting and event
 * translation only.
 *
 * The 5200 has no disks, tape, keyboard, SIO, PIA or save states, and a
 * FujiNet cartridge always sits in the slot: CONFIG is its resident image,
 * and what the user opens is served by that cartridge as a game. There is no
 * reset line to the cartridge, so only a POWER CYCLE brings CONFIG back.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef A52SESSION_H
#define A52SESSION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The widest picture (atari800's whole 384x240 overscan area; the default
 * visible area is 336x224 NTSC / 336x240 PAL). */
#define A52SESSION_FB_MAX_WIDTH  384
#define A52SESSION_FB_MAX_HEIGHT 240

/* FujiNet's BoIP listener (loopback; the cartridge DIALS it, as on every
 * sibling) and its web admin UI. Siblings use 11500-11523. */
#define A52SESSION_BOIP_PORT  11524
#define A52SESSION_WEBUI_PORT 11525

#define A52SESSION_AUDIO_RATE 48000

/* Highlights (Map target, held keys, debugger PC line, status dot): the
 * orange of the console's POWER legend. A fill colour with dark text on it;
 * on light themes text in the accent uses the dark variant. */
#define A52SESSION_ACCENT_RGB      0xD2642B
#define A52SESSION_ACCENT_DARK_RGB 0x9C4318

typedef struct a52session a52session;
typedef struct a52debug a52debug;

/* All optional (NULL = default).
 *  config_dir   default $XDG_CONFIG_HOME/fujinet-go-atari5200 (%APPDATA%)
 *  data_dir     default $XDG_DATA_HOME/fujinet-go-atari5200 (%LOCALAPPDATA%)
 *  fujinet_lib  libfujinet path; "" disables FujiNet
 *  fujinet_runtime_src  pristine fnconfig.ini + data/ + SD/ (macOS bundle) */
typedef struct {
    const char *config_dir;
    const char *data_dir;
    const char *fujinet_lib;
    const char *fujinet_runtime_src;
} a52session_paths;

a52session *a52session_new(const a52session_paths *paths);
void a52session_free(a52session *s);

/* ---- settings ------------------------------------------------------------ */
int         a52session_get_int(a52session *s, const char *key, int def);
void        a52session_set_int(a52session *s, const char *key, int value);
const char *a52session_get_str(a52session *s, const char *key, const char *def);
void        a52session_set_str(a52session *s, const char *key, const char *value);
void        a52session_settings_flush(a52session *s);

/* ---- Preferences, as a table every frontend builds its dialog from -------
 * Values live in the settings store under .key; after changing any, call
 * a52session_apply_settings. Pages: "Video", "Audio", "Input", "BIOS",
 * "FujiNet". */
typedef enum {
    A52_PREF_BOOL = 0, A52_PREF_CHOICE, A52_PREF_INT, A52_PREF_FILE,
    A52_PREF_DIR, A52_PREF_TEXT
} a52_pref_type;

/* When a change takes effect. */
typedef enum {
    A52_APPLY_LIVE = 0,    /* at once */
    A52_APPLY_POWER,       /* power-cycles the console (apply_settings does it) */
    A52_APPLY_RESTART      /* restarts the session (apply_settings does it) */
} a52_apply;

typedef struct {
    const char *key;        /* settings key */
    const char *page;
    const char *group;      /* group title on the page */
    const char *label;
    const char *help;       /* subtitle; may be NULL */
    int type;               /* a52_pref_type */
    int def;                /* BOOL/CHOICE/INT default */
    int min, max, step;     /* INT */
    int applies;            /* a52_apply */
} a52_pref;

int a52session_pref_count(void);
const a52_pref *a52session_pref(int i);
int a52session_pref_find(const char *key);     /* index or -1 */
/* CHOICE labels can depend on other settings (the imported BIOS images):
 * label c of pref i, or NULL past the end. */
const char *a52session_pref_choice(a52session *s, int i, int c);
/* CHOICE values: always through these (some are stored by name). */
int  a52session_pref_get_choice(a52session *s, int i);
void a52session_pref_set_choice(a52session *s, int i, int c);
/* 0 = shown greyed (e.g. a BIOS that is not imported). */
int a52session_pref_enabled(a52session *s, int i);
int a52session_pref_choice_enabled(a52session *s, int i, int c);
/* TEXT prefs: the current text ("" if never set) and a validated set. A text
 * the pref does not accept is refused (-1, nothing stored, the reason in err
 * when given); 0 stores it. */
const char *a52session_pref_get_text(a52session *s, int i);
int  a52session_pref_set_text(a52session *s, int i, const char *text,
                               char *err, int errsz);
/* What a TEXT pref shows while empty (its default), or NULL. */
const char *a52session_pref_placeholder(int i);

/* Applies every changed setting; returns the highest a52_apply it needed. */
int a52session_apply_settings(a52session *s);

/* ---- lifecycle ----------------------------------------------------------- */
typedef struct {
    int enable_fujinet;     /* start the in-process FujiNet runtime */
    int enable_audio;       /* open the SDL audio device */
    int enable_gamepad;     /* start the SDL gamepad thread */
    int external_fujinet;   /* 1: don't start it; the cartridge DIALS
                             * fujinet_addr (another FujiNet instance) */
    const char *fujinet_addr; /* "host:port" or "port" the cartridge dials
                               * (external only); NULL/"" = 127.0.0.1:11524 */
    const char *cart_path;  /* a game cartridge to boot instead of CONFIG
                             * (default: the "cart" setting, "" = CONFIG) */
    int sync_cart;          /* 1: the cartridge services its mailbox inline
                             * (deterministic; the tests) instead of on a
                             * worker thread */
} a52session_start_opts;

/* Fills opts from the settings store (enable_fujinet, enable_audio,
 * enable_gamepad, fujinet_external, fujinet_external_addr, cart). */
void a52session_default_opts(a52session *s, a52session_start_opts *opts);
/* Starts FujiNet (if enabled), then the console. FujiNet listens and the
 * cartridge dials, so the listener comes first. FujiNet failing to start is
 * not fatal: the cartridge reports its link down. Returns 0 or -1 with
 * a52session_last_error(). */
int  a52session_start(a52session *s, const a52session_start_opts *opts);
void a52session_stop(a52session *s);
int  a52session_is_running(const a52session *s);
const char *a52session_last_error(const a52session *s);

/* ---- power ---------------------------------------------------------------- */
/* Power cycle: the console AND the cartridge power off and on, so CONFIG is
 * back (an opened game is ejected and the "cart" setting cleared), as when
 * the real console's POWER switch is thrown. The primary action; "Reboot to
 * CONFIG" in the menus. */
int  a52session_power_cycle(a52session *s);
int  a52session_reboot_to_config(a52session *s);   /* same as power_cycle */
/* The controller's RESET key, pressed and released for a few frames (menus,
 * the controllers window; a held key goes through a52session_press with
 * A52_TARGET_KEY(A52_KEY_RESET)). The BIOS restarts the cartridge's CURRENT
 * image: the cartridge never sees a reset. */
int  a52session_reset(a52session *s);
void a52session_set_warp(a52session *s, int on);
int  a52session_warp(a52session *s);
/* Pause the console (the picture holds; the debugger's Run/Stop is separate). */
void a52session_set_paused(a52session *s, int paused);
int  a52session_is_paused(const a52session *s);

/* ---- video --------------------------------------------------------------- */
/* XRGB8888 into dst (A52SESSION_FB_MAX_WIDTH * _MAX_HEIGHT), its size and
 * the pixel aspect x1000 (always 1000: square). Returns 1 if a new frame was
 * copied; *serial 0 forces a copy. */
int  a52session_copy_frame(a52session *s, uint32_t *dst, int *width, int *height,
                            int *par_milli, uint64_t *serial_inout);
int  a52session_refresh_rate(a52session *s);  /* x1000: 59923 or 49860 */
/* Ask to be told when a new frame is ready, for a frontend with no frame
 * clock of its own: cb(ctx) runs on the thread that publishes frames (the
 * emulator's) after each is stored; a frame identical to the previous one is
 * not published again. Keep it quick (post an event) and do not call into the
 * session from it. NULL stops the calls and returns only after any call in
 * flight has finished, so ctx may be freed right after. */
typedef void (*a52session_frame_cb)(void *ctx);
void a52session_set_frame_callback(a52session *s, a52session_frame_cb cb, void *ctx);
/* Frames the machine has run, including the ones identical to their
 * predecessor (not published again): use it to ask "is the machine running",
 * not "is the picture changing". */
uint64_t a52session_frame_count(a52session *s);
void a52session_notify_vsync(a52session *s, int64_t frame_time_ns);

/* ---- audio --------------------------------------------------------------- */
int  a52session_render_audio(a52session *s, float *stereo, int nframes);
void a52session_set_volume(a52session *s, int percent);

/* ---- input targets ----------------------------------------------------------
 * Four controller ports. Per port: the analog stick (digital directions drive
 * it to its ends; a52session_stick_set / the gamepads' left stick are analog),
 * FIRE (the two side buttons: one wire) and FIRE2 (the two top buttons).
 * Then the console keys: the 12-key pad 0-9 * #, START, PAUSE and RESET.
 * atari800 (like MAME) scans ONE keypad and ONE "second fire" for all four
 * controllers (the hardware's per-controller select is not emulated), so the
 * keys and FIRE2 are common: pressing them on any controller is the same.
 * Frontends draw one keypad/key panel and a joystick per port. */
typedef enum {
    A52_ACT_UP = 0, A52_ACT_DOWN, A52_ACT_LEFT, A52_ACT_RIGHT, A52_ACT_FIRE,
    A52_ACT_FIRE2, A52_ACT_PER_PORT
} a52_action;
#define A52_PORTS 4

/* Console keys, A52_TARGET_KEY(k): */
typedef enum {
    A52_KEY_0 = 0, A52_KEY_1, A52_KEY_2, A52_KEY_3, A52_KEY_4, A52_KEY_5,
    A52_KEY_6, A52_KEY_7, A52_KEY_8, A52_KEY_9,
    A52_KEY_STAR, A52_KEY_HASH,
    A52_KEY_START, A52_KEY_PAUSE, A52_KEY_RESET,
    A52_KEY_COUNT
} a52_key;

typedef enum {
    A52_SYSACT_POWER = 0,      /* power cycle (reboot to CONFIG) */
    A52_SYSACT_PAUSE,          /* stop in the debugger */
    A52_SYSACT_WARP,           /* toggle warp */
    A52_SYSACT_COUNT
} a52_sysaction;

#define A52_TARGET_PORT(port, act) ((port) * A52_ACT_PER_PORT + (act))
#define A52_TARGET_KEY(k)          (A52_PORTS * A52_ACT_PER_PORT + (k))
#define A52_TARGET_SYSACT(sa)      (A52_PORTS * A52_ACT_PER_PORT + A52_KEY_COUNT + (sa))
#define A52_TARGET_COUNT           (A52_PORTS * A52_ACT_PER_PORT + A52_KEY_COUNT + A52_SYSACT_COUNT)

const char *a52_target_name(int target);       /* "Controller 1: Up", "Key 5", "RESET" */
const char *a52_target_short_name(int target); /* "Up", "5", "#", "START" */
const char *a52_key_label(int key);            /* "0".."9","*","#","START","PAUSE","RESET" */

/* Apply one control directly (keyboard bindings, on-screen buttons). */
void a52session_press(a52session *s, int target, int down);
void a52session_sysaction(a52session *s, int sysact);
void a52session_release_all(a52session *s);
/* What the console sees held on a port right now (keyboard + gamepads'
 * digital inputs), 1 << a52_action; and which console keys are held,
 * 1 << a52_key. For the controllers window's highlights. */
unsigned a52session_buttons_held(a52session *s, int port);
unsigned a52session_keys_held(a52session *s);
/* The analog stick of `port` right now: x,y -128..127 (+ right, + DOWN, SDL's
 * and the screen's convention); 0,0 = centre. Includes the digital
 * directions (as full deflection) and the gamepad sticks. */
void a52session_stick_get(a52session *s, int port, int *x, int *y);
/* Drive the analog stick of `port` from a pointer (the controllers window's
 * mouse drag, the picture's mouse mode): x,y -128..127 (+ right, + DOWN),
 * clamped; 0,0 releases it. One more source beside the gamepads (strongest
 * deflection wins). Call it with 0,0 when the drag ends. */
void a52session_stick_set(a52session *s, int port, int x, int y);
/* On (the default; setting "analog_stick"): a gamepad's left stick is analog
 * (radial dead zone 12%). Off: an eight-way switch driving the digital
 * directions. Takes effect at once. */
void a52session_set_analog(a52session *s, int on);

/* The controller as printed, for the on-screen panel: the 12-key pad (3x4)
 * and START / PAUSE / RESET, one entry per cap in cap units (1.0 = a keypad
 * key) from the top left. `key` is a52_key (its binding target is
 * A52_TARGET_KEY(key)). Static; valid for the process. */
typedef enum { A52_KEYCAP_NORMAL = 0, A52_KEYCAP_FUNCTION } a52_keycap_kind;
typedef struct {
    const char *label;
    int key;                 /* a52_key */
    float x, y, w, h;
    int kind;                /* a52_keycap_kind */
} a52_keycap;
const a52_keycap *a52session_keyboard_layout(int *n, float *width, float *height);

/* ---- keyboard translation ---------------------------------------------------
 * keysym: X11-style (below). Returns 1 if the key drives a target (applied
 * or released), 0 if it should be ignored. System actions are reported
 * through a52session_key_sysaction and left to the frontend. */
int  a52session_key(a52session *s, uint32_t keysym, int down);
int  a52session_key_sysaction(a52session *s, uint32_t keysym);

enum {
    A52_KEYSYM_NONE = 0, A52_KEYSYM_SPACE = 0x20,
    A52_KEYSYM_BACKSPACE = 0xff08, A52_KEYSYM_TAB = 0xff09,
    A52_KEYSYM_RETURN = 0xff0d, A52_KEYSYM_PAUSE = 0xff13,
    A52_KEYSYM_SCROLL_LOCK = 0xff14, A52_KEYSYM_ESCAPE = 0xff1b,
    A52_KEYSYM_HOME = 0xff50, A52_KEYSYM_LEFT = 0xff51, A52_KEYSYM_UP = 0xff52,
    A52_KEYSYM_RIGHT = 0xff53, A52_KEYSYM_DOWN = 0xff54,
    A52_KEYSYM_PAGE_UP = 0xff55, A52_KEYSYM_PAGE_DOWN = 0xff56,
    A52_KEYSYM_END = 0xff57, A52_KEYSYM_INSERT = 0xff63,
    A52_KEYSYM_MENU = 0xff67, A52_KEYSYM_BREAK = 0xff6b,
    A52_KEYSYM_KP_ENTER = 0xff8d, A52_KEYSYM_KP_MULTIPLY = 0xffaa,
    A52_KEYSYM_KP_ADD = 0xffab, A52_KEYSYM_KP_SUBTRACT = 0xffad,
    A52_KEYSYM_KP_PERIOD = 0xffae, A52_KEYSYM_KP_DIVIDE = 0xffaf,
    A52_KEYSYM_KP_0 = 0xffb0, A52_KEYSYM_KP_1, A52_KEYSYM_KP_2,
    A52_KEYSYM_KP_3, A52_KEYSYM_KP_4, A52_KEYSYM_KP_5, A52_KEYSYM_KP_6,
    A52_KEYSYM_KP_7, A52_KEYSYM_KP_8, A52_KEYSYM_KP_9,
    A52_KEYSYM_F1 = 0xffbe, A52_KEYSYM_F2, A52_KEYSYM_F3, A52_KEYSYM_F4,
    A52_KEYSYM_F5, A52_KEYSYM_F6, A52_KEYSYM_F7, A52_KEYSYM_F8,
    A52_KEYSYM_F9, A52_KEYSYM_F10, A52_KEYSYM_F11, A52_KEYSYM_F12,
    A52_KEYSYM_LSHIFT = 0xffe1, A52_KEYSYM_RSHIFT = 0xffe2,
    A52_KEYSYM_LCTRL = 0xffe3, A52_KEYSYM_RCTRL = 0xffe4,
    A52_KEYSYM_CAPS_LOCK = 0xffe5,
    A52_KEYSYM_LALT = 0xffe9, A52_KEYSYM_RALT = 0xffea,
    A52_KEYSYM_LSUPER = 0xffeb, A52_KEYSYM_RSUPER = 0xffec,
    A52_KEYSYM_DELETE = 0xffff
};
/* Printable keys are their lower-case ASCII ('a', '1', '-', ...). */
uint32_t a52session_keysym_from_win_scancode(unsigned scancode, int extended);
uint32_t a52session_keysym_from_evdev(unsigned code);
uint32_t a52session_keysym_from_macos_keycode(unsigned keycode);
int a52session_keysym_name(uint32_t keysym, char *dst, int dstsz);

/* ---- bindings ------------------------------------------------------------ */
typedef enum {
    A52_PAD_BTN_NONE = -1,
    A52_PAD_BTN_SOUTH = 0, A52_PAD_BTN_EAST, A52_PAD_BTN_WEST,
    A52_PAD_BTN_NORTH, A52_PAD_BTN_BACK, A52_PAD_BTN_GUIDE,
    A52_PAD_BTN_START, A52_PAD_BTN_LEFT_STICK, A52_PAD_BTN_RIGHT_STICK,
    A52_PAD_BTN_LEFT_SHOULDER, A52_PAD_BTN_RIGHT_SHOULDER,
    A52_PAD_BTN_DPAD_UP, A52_PAD_BTN_DPAD_DOWN, A52_PAD_BTN_DPAD_LEFT,
    A52_PAD_BTN_DPAD_RIGHT,
    A52_PAD_BTN_LEFT_TRIGGER, A52_PAD_BTN_RIGHT_TRIGGER,
    A52_PAD_BTN_COUNT,
    A52_PAD_BTN_RAW_BASE = 64, A52_PAD_BTN_RAW_LAST = 127,
    A52_PAD_HAT_BASE = 192, A52_PAD_HAT_LAST = 255
} a52_pad_button;
#define A52_PAD_HAT_DIRS 8
#define A52_PAD_BTN_IS_NAMED(b) ((b) >= 0 && (b) < A52_PAD_BTN_COUNT)
#define A52_PAD_BTN_IS_RAW(b)   ((b) >= A52_PAD_BTN_RAW_BASE && (b) <= A52_PAD_BTN_RAW_LAST)
#define A52_PAD_BTN_IS_HAT(b)   ((b) >= A52_PAD_HAT_BASE && (b) <= A52_PAD_HAT_LAST)

typedef struct {
    uint32_t keysym;        /* 0 = no key */
    int      button;        /* a52_pad_button */
} a52_binding;

a52_binding a52session_binding_get(a52session *s, int target);
void a52session_binding_set_key(a52session *s, int target, uint32_t keysym,
                                 char *stolen, int stolensz);
void a52session_binding_set_button(a52session *s, int target, int button,
                                    char *stolen, int stolensz);
void a52session_bindings_reset(a52session *s);
const char *a52_pad_button_name(int button);
int  a52session_target_for_key(a52session *s, uint32_t keysym);
int  a52session_target_for_button(a52session *s, int port, int button);
void a52session_gamepad_capture_begin(a52session *s);
void a52session_gamepad_capture_cancel(a52session *s);
int  a52session_gamepad_capture_poll(a52session *s, int *button);

/* ---- gamepads (hot-plugged) ----------------------------------------------- */
int  a52session_gamepad_count(a52session *s);
int  a52session_gamepad_name(a52session *s, int idx, char *dst, int dstsz);
void a52session_gamepad_assign(a52session *s, int idx, int port); /* -1 auto */
int  a52session_gamepad_assignment(a52session *s, int idx);
int  a52session_gamepad_effective_port(a52session *s, int idx);
unsigned a52session_gamepad_generation(a52session *s);
int  a52session_gamepad_last_event(a52session *s, char *dst, int dstsz);

/* Cross-thread system actions (the gamepad thread posts; a UI timer takes). */
void a52session_sysaction_post(a52session *s, int sysact);
int  a52session_sysaction_take(a52session *s, int *out);

/* ---- media: game cartridges, served by the FujiNet cartridge ------------------ */
typedef enum {
    A52_MEDIA_UNKNOWN = 0,
    A52_MEDIA_CARTRIDGE,   /* .a52 .bin .rom .car, or a .zip of exactly one */
    A52_MEDIA_BIOS         /* a console BIOS image: file, .zip or folder */
} a52_media_kind;
/* Classifies a path by content: a BIOS (recognised by size+CRC, a zip or a
 * folder holding any) is BIOS; a file/zip the cartridge planner accepts is a
 * cartridge. */
int a52session_media_kind(a52session *s, const char *path);

/* Open anything: a cartridge is served by the FujiNet cartridge and the
 * console is power cycled onto it (a FujiNet app that carries the "FUJI"
 * claim keeps the mailbox live; a plain game runs alone); a BIOS file / zip /
 * folder is imported instead. msg says what happened. Returns 0, or -1 with
 * the reason in msg. The cartridge path is remembered in the "cart" setting
 * (it boots again next start; a power cycle clears it). */
int a52session_open(a52session *s, const char *path, char *msg, int msgsz);
int a52session_load_cart(a52session *s, const char *path, char *msg, int msgsz);
/* Eject = power cycle to CONFIG. */
int a52session_eject(a52session *s);
const char *a52session_cart_path(const a52session *s);   /* "" = CONFIG */
/* 1 if the image can run on the cartridge (a52map planner); else 0 + why. */
int a52session_check_cart(const char *path, char *why, int whysz);
/* 1 if it is a FujiNet app (carries the FUJI claim: the mailbox stays live). */
int a52session_cart_is_app(const char *path);
/* "FujiNet CONFIG", "FujiNet app (32 KB)", "Cartridge: Super Cart 128 KB (CRC
 * 12345678)" ...; returns the length. */
int a52session_cart_info(a52session *s, char *dst, int dstsz);
/* Copies into the FujiNet SD folder so CONFIG can boot it (a zip of one is
 * unpacked; any other file as is). */
int a52session_import_to_sd(a52session *s, const char *src_path,
                             char *dest_out, int dest_sz);
/* Routes a dropped file: BIOS -> imported (msg), cartridge -> opened,
 * anything else -> the SD folder. Returns what it was (a52_media_kind, 0 for
 * the SD folder) or -1 with msg. */
int a52session_open_or_import(a52session *s, const char *path, char *msg, int msgsz);

/* ---- BIOS images --------------------------------------------------------------
 * The console BIOS is atari800's built-in Altirra 5200 OS (free) unless the
 * user imports a real one. Imports are by size + CRC-32 from a file, a ZIP
 * (MAME's a5200.zip / a5200a.zip work) or a FOLDER (searched recursively,
 * every recognised file taken, the rest ignored); written to <data>/roms/.
 * The BIOS in use is the "bios" preference (Built-in / 4-port / 2-port; only
 * imported ones selectable). A missing BIOS is NOT an error. */
typedef struct {
    const char *key;        /* settings key holding the imported path */
    const char *name;       /* "Atari 5200 BIOS (4-port)" */
    const char *group;      /* "Console BIOS" */
    uint32_t size;
} a52_rom_slot;
int a52session_rom_slot_count(void);
const a52_rom_slot *a52session_rom_slot(int i);
int a52session_rom_available(a52session *s, int slot);
/* Identify a file/zip/folder: the slots it can fill (best first); 0 if none. */
int a52session_rom_probe(a52session *s, const char *path, int *slots, int max);
/* Import: slot < 0 takes everything recognised in path (file, zip, folder);
 * slot >= 0 imports into that slot only (the per-slot Import button; the
 * file must be that slot's image). msg says what was imported and ignored.
 * Returns the number of images imported, or -1 (nothing recognised, with
 * the reason in msg). */
int a52session_rom_import(a52session *s, const char *path, int slot,
                           char *msg, int msgsz);
/* Removes an imported image (the BIOS falls back to the built-in one). */
int a52session_rom_remove(a52session *s, int slot);
/* "Built-in Altirra 5200 OS", "Atari 5200 BIOS (4-port)" ...: the BIOS the
 * running console uses. */
int a52session_bios_name(a52session *s, char *dst, int dstsz);

/* ---- FujiNet ------------------------------------------------------------------- */
/* Parses the "Use external FujiNet" address field: "host:port" (host an IPv4
 * literal or "localhost") or a bare "port". Empty text means the default,
 * 127.0.0.1:11524. Returns 0 and fills host/port, or -1 when the text is not
 * an address:port (nothing is written then). */
int         a52session_parse_hostport(const char *text, char *host, int hostsz,
                                       int *port);
int         a52session_fujinet_running(const a52session *s);
/* 1 when the session was started to dial an external FujiNet (no runtime of
 * ours; fujinet_running stays 0). */
int         a52session_fujinet_external(const a52session *s);
/* "host:port" the cartridge dials (the running session's, or the built-in
 * FujiNet's 127.0.0.1:11524 before the first start). */
const char *a52session_fujinet_endpoint(const a52session *s);
/* The cartridge's link to FujiNet: 1 up, 0 down, -1 not running. */
int         a52session_fujinet_link(a52session *s);
/* Status text from the cartridge ("connected", "link down", "loading 42%",
 * "game running; mailbox closed" ...). Returns the length. */
int         a52session_cart_status(a52session *s, char *dst, int dstsz);
/* 1 once anything other than CONFIG is in the cartridge. */
int         a52session_cart_booted_game(a52session *s);
const char *a52session_fujinet_webui_url(const a52session *s);
int         a52session_fujinet_copy_log(a52session *s, char *dst, int max);
int         a52session_fujinet_restart(a52session *s);
unsigned    a52session_fujinet_generation(a52session *s);
int         a52session_fujinet_last_event(a52session *s, char *dst, int dstsz);

/* ---- paths and the debugger ------------------------------------------------- */
const char *a52session_config_path(const a52session *s);
const char *a52session_data_path(const a52session *s);
const char *a52session_roms_path(const a52session *s);
const char *a52session_media_path(const a52session *s);   /* where games are kept */
const char *a52session_sd_path(const a52session *s);

/* A short status line: BIOS, TV system, cartridge, FujiNet link. */
int a52session_status(a52session *s, char *dst, int dstsz);
/* A CIM or other stop the user should hear about (one-shot); 0 if none. */
int a52session_take_notice(a52session *s, char *dst, int dstsz);

a52debug *a52session_debugger(a52session *s);

#ifdef __cplusplus
}
#endif

#endif /* A52SESSION_H */
