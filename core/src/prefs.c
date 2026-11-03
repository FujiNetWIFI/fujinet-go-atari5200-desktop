/*
 * prefs.c -- the Preferences table and how each setting reaches atari800.
 *
 * Every frontend builds its dialog from s_prefs[]. Values live in the
 * settings store; prefs_emit_options() turns them into atari800 config
 * options (applied with CFG_ApplyOption inside Atari800_Initialise).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "session_internal.h"

/* ---- the table ------------------------------------------------------------------ */

static const char *const ch_tv[] = {"NTSC", "PAL", NULL};
static const char *const ch_preset[] = {"Standard", "Deep black", "Vibrant", NULL};
static const char *const ch_area[] = {"Narrow (320 \xc3\x97 200)", "TV (336 \xc3\x97 224/240)",
                                      "Overscan (384 \xc3\x97 240)", NULL};
static const char *const ch_aspect[] = {"TV (4:3)", "Square pixels", NULL};
static const char *const ch_autofire[] = {"Off", "While fire is held", "Always", NULL};

/* "bios" values */
enum { BIOS_BUILTIN = 0, BIOS_4PORT, BIOS_2PORT, BIOS_COUNT };
static const char *const bios_labels[BIOS_COUNT] = {
    "Built-in Altirra 5200 OS", "Atari 5200 BIOS (4-port console)",
    "Atari 5200 BIOS (2-port console)"};

#define P_BOOL(k, pg, gr, lb, hp, d, ap) {k, pg, gr, lb, hp, A52_PREF_BOOL, d, 0, 1, 1, ap}
#define P_CHOICE(k, pg, gr, lb, hp, d, ap) {k, pg, gr, lb, hp, A52_PREF_CHOICE, d, 0, 0, 0, ap}
#define P_INT(k, pg, gr, lb, hp, d, mn, mx, st, ap) {k, pg, gr, lb, hp, A52_PREF_INT, d, mn, mx, st, ap}
#define P_TEXT(k, pg, gr, lb, hp, ap) {k, pg, gr, lb, hp, A52_PREF_TEXT, 0, 0, 0, 0, ap}
#define P_FILE(k, pg, gr, lb, hp, ap) {k, pg, gr, lb, hp, A52_PREF_FILE, 0, 0, 0, 0, ap}

static const a52_pref s_prefs[] = {
    /* Video */
    P_CHOICE("tv", "Video", "Console", "TV system",
             "The 5200 is an NTSC console; PAL changes atari800's timing and palette only",
             0, A52_APPLY_POWER),
    P_CHOICE("palette_preset", "Video", "Colour", "Palette", NULL, 0, A52_APPLY_LIVE),
    P_INT("saturation", "Video", "Colour", "Saturation", NULL, 0, -100, 100, 1, A52_APPLY_LIVE),
    P_INT("contrast", "Video", "Colour", "Contrast", NULL, 0, -100, 100, 1, A52_APPLY_LIVE),
    P_INT("brightness", "Video", "Colour", "Brightness", NULL, 0, -100, 100, 1, A52_APPLY_LIVE),
    P_INT("gamma", "Video", "Colour", "Gamma", NULL, 0, -100, 100, 1, A52_APPLY_LIVE),
    P_INT("hue", "Video", "Colour", "Tint", NULL, 0, -100, 100, 1, A52_APPLY_LIVE),
    P_FILE("palette_file", "Video", "Colour", "External palette (.act)", NULL, A52_APPLY_LIVE),
    P_CHOICE("area", "Video", "Picture", "Visible area", NULL, 1, A52_APPLY_LIVE),
    P_CHOICE("aspect", "Video", "Picture", "Aspect", NULL, 0, A52_APPLY_LIVE),
    P_BOOL("smooth", "Video", "Picture", "Smooth scaling", NULL, 0, A52_APPLY_LIVE),

    /* Audio */
    P_INT("volume", "Audio", "Sound", "Volume", NULL, 80, 0, 100, 5, A52_APPLY_LIVE),
    P_BOOL("mz_pokey", "Audio", "Sound", "High-fidelity POKEY", NULL, 1, A52_APPLY_POWER),

    /* Input */
    P_BOOL("analog_stick", "Input", "Controllers", "Gamepad sticks are analog",
           "The 5200's controllers are analog: off makes the left stick an eight-way switch",
           1, A52_APPLY_LIVE),
    P_CHOICE("autofire", "Input", "Controllers", "Autofire", NULL, 0, A52_APPLY_LIVE),

    /* BIOS */
    P_CHOICE("bios", "BIOS", "Console BIOS", "BIOS",
             "The built-in Altirra OS runs everything; a real BIOS can be imported "
             "(Import BIOS) and chosen here",
             BIOS_BUILTIN, A52_APPLY_POWER),

    /* FujiNet */
    P_BOOL("enable_fujinet", "FujiNet", "FujiNet", "FujiNet",
           "The built-in FujiNet, behind the cartridge", 1, A52_APPLY_RESTART),
    P_BOOL("fujinet_external", "FujiNet", "External FujiNet", "Use external FujiNet",
           "Skip the built-in FujiNet and dial another instance",
           0, A52_APPLY_RESTART),
    P_TEXT("fujinet_external_addr", "FujiNet", "External FujiNet", "Address:port",
           "That instance's BoIP listener, e.g. 192.168.1.20:11524. The cartridge "
           "dials it",
           A52_APPLY_RESTART),
    P_BOOL("enable_audio", "FujiNet", "Host", "Sound output", NULL, 1, A52_APPLY_RESTART),
    P_BOOL("enable_gamepad", "FujiNet", "Host", "Gamepads", NULL, 1, A52_APPLY_RESTART),
};

#define N_PREFS ((int)(sizeof s_prefs / sizeof s_prefs[0]))

int a52session_pref_count(void)
{
    return N_PREFS;
}

const a52_pref *a52session_pref(int i)
{
    return i >= 0 && i < N_PREFS ? &s_prefs[i] : NULL;
}

int a52session_pref_find(const char *key)
{
    int i;
    for (i = 0; i < N_PREFS; i++)
        if (!strcmp(s_prefs[i].key, key))
            return i;
    return -1;
}

static int pref_is(int i, const char *key)
{
    return i >= 0 && i < N_PREFS && !strcmp(s_prefs[i].key, key);
}

static const char *const *static_choices(const char *key)
{
    static const struct { const char *key; const char *const *ch; } t[] = {
        {"tv", ch_tv}, {"palette_preset", ch_preset}, {"area", ch_area},
        {"aspect", ch_aspect}, {"autofire", ch_autofire},
    };
    size_t i;
    for (i = 0; i < sizeof t / sizeof t[0]; i++)
        if (!strcmp(t[i].key, key))
            return t[i].ch;
    return NULL;
}

const char *a52session_pref_choice(a52session *s, int i, int c)
{
    const char *const *ch;
    (void)s;
    if (i < 0 || i >= N_PREFS || c < 0)
        return NULL;
    if (pref_is(i, "bios"))
        return c < BIOS_COUNT ? bios_labels[c] : NULL;
    ch = static_choices(s_prefs[i].key);
    if (!ch)
        return NULL;
    while (c > 0 && *ch) {
        ch++;
        c--;
    }
    return *ch;
}

static int bios_slot_of(int choice)
{
    return choice == BIOS_4PORT ? ROM_5200_4PORT : choice == BIOS_2PORT ? ROM_5200_2PORT : -1;
}

/* The choice in effect: an imported BIOS that has gone missing falls back to
 * the built-in one. */
static int bios_effective(a52session *s)
{
    int c = a52session_get_int(s, "bios", BIOS_BUILTIN);
    if (c < 0 || c >= BIOS_COUNT)
        return BIOS_BUILTIN;
    if (c != BIOS_BUILTIN && !a52session_rom_available(s, bios_slot_of(c)))
        return BIOS_BUILTIN;
    return c;
}

int prefs_bios_slot(struct a52session *s)
{
    return bios_slot_of(bios_effective(s));
}

int a52session_pref_get_choice(a52session *s, int i)
{
    if (i < 0 || i >= N_PREFS)
        return 0;
    if (pref_is(i, "bios"))
        return bios_effective(s);
    return a52session_get_int(s, s_prefs[i].key, s_prefs[i].def);
}

void a52session_pref_set_choice(a52session *s, int i, int c)
{
    if (i >= 0 && i < N_PREFS)
        a52session_set_int(s, s_prefs[i].key, c);
}

int a52session_pref_enabled(a52session *s, int i)
{
    const char *k;
    if (i < 0 || i >= N_PREFS)
        return 0;
    k = s_prefs[i].key;
    if (!strcmp(k, "fujinet_external"))
        return a52session_get_int(s, "enable_fujinet", 1);
    if (!strcmp(k, "fujinet_external_addr"))
        return a52session_get_int(s, "enable_fujinet", 1) &&
               a52session_get_int(s, "fujinet_external", 0);
    return 1;
}

const char *a52session_pref_get_text(a52session *s, int i)
{
    if (i < 0 || i >= N_PREFS || s_prefs[i].type != A52_PREF_TEXT)
        return "";
    return a52session_get_str(s, s_prefs[i].key, "");
}

int a52session_pref_set_text(a52session *s, int i, const char *text, char *err, int errsz)
{
    if (i < 0 || i >= N_PREFS || s_prefs[i].type != A52_PREF_TEXT || !text)
        return -1;
    if (!strcmp(s_prefs[i].key, "fujinet_external_addr")) {
        char host[64];
        int port;
        if (a52session_parse_hostport(text, host, sizeof host, &port) != 0) {
            if (err && errsz > 0)
                snprintf(err, (size_t)errsz,
                         "\"%s\" is not an address:port (example: 127.0.0.1:11524)", text);
            return -1;
        }
    }
    a52session_set_str(s, s_prefs[i].key, text);
    return 0;
}

const char *a52session_pref_placeholder(int i)
{
    if (i >= 0 && i < N_PREFS && !strcmp(s_prefs[i].key, "fujinet_external_addr"))
        return "127.0.0.1:11524";
    return NULL;
}

int a52session_pref_choice_enabled(a52session *s, int i, int c)
{
    if (pref_is(i, "bios")) {
        if (c < 0 || c >= BIOS_COUNT)
            return 0;
        return c == BIOS_BUILTIN || a52session_rom_available(s, bios_slot_of(c));
    }
    return 1;
}

/* ---- to atari800 ------------------------------------------------------------- */

typedef void (*emit_fn)(void *u, const char *key, const char *val);

static void emitf(emit_fn emit, void *u, const char *key, const char *fmt, ...)
{
    char buf[1100];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    emit(u, key, buf);
}

/* The machine (always a 5200), its BIOS and what is read only at start. */
void prefs_emit_options(struct a52session *s, emit_fn emit, void *u)
{
    int slot = prefs_bios_slot(s);

    emit(u, "MACHINE_TYPE", "Atari 5200");
    emit(u, "RAM_SIZE", "16");
    emit(u, "DEFAULT_TV_MODE", a52session_get_int(s, "tv", 0) ? "PAL" : "NTSC");
    if (slot >= 0) {
        /* atari800 files the path under whichever 5200 revision its CRC says */
        emit(u, "5200_ROM", roms_path(s, slot));
        emit(u, "OS_5200_VERSION", slot == ROM_5200_4PORT ? "ORIG" : "A");
    }
    else {
        emit(u, "OS_5200_VERSION", "ALTIRRA");
    }
    emitf(emit, u, "ENABLE_NEW_POKEY", "%d", a52session_get_int(s, "mz_pokey", 1));
    emitf(emit, u, "STEREO_POKEY", "0");
    emitf(emit, u, "SPEAKER_SOUND", "0");
    prefs_emit_live(s, emit, u);
}

/* What can change while running (also part of every start). */
void prefs_emit_live(struct a52session *s, emit_fn emit, void *u)
{
    const char *pal = a52session_get_str(s, "palette_file", "");

    emit(u, "COLOURS_NTSC_EXTERNAL_PALETTE", pal);
    emit(u, "COLOURS_PAL_EXTERNAL_PALETTE", pal);
    emitf(emit, u, "COLOURS_NTSC_EXTERNAL_PALETTE_LOADED", "%d", *pal != 0);
    emitf(emit, u, "COLOURS_PAL_EXTERNAL_PALETTE_LOADED", "%d", *pal != 0);
}
