/*
 * The main application window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "a52session.h"

#define A52_TYPE_WINDOW (a52_window_get_type())
G_DECLARE_FINAL_TYPE(A52Window, a52_window, A52, WINDOW, AdwApplicationWindow)

GtkWidget *a52_window_new(AdwApplication *app, a52session *session);
/* main.c: the icon name to use (the installed id, or the in-tree art). */
const char *a52_icon_name(void);
void a52_window_toast(A52Window *self, const char *text);

/* The Help context the main window shows now: "main", or "prefs/<page>"
 * while Preferences is open. */
const char *a52_window_help_context(A52Window *self);

/* Starts the machine (FujiNet first). */
void a52_window_start(A52Window *self);

/* A key pressed or released with this window, or one of its panels, active:
 * the window's own keys (F9, F11, F12, Shift+F5, the CTRL+SHIFT shortcuts),
 * else the Atari's. keycode is GDK's hardware keycode (evdev + 8), which
 * keeps press and release paired whatever the modifiers did in between.
 * TRUE when it was used. */
gboolean a52_window_key(A52Window *self, guint keyval, guint keycode,
                         GdkModifierType state, gboolean down);

/* A dropped or command-line file: Open's routing (a cartridge is served by
 * the FujiNet cartridge and the console power-cycles onto it, a BIOS file,
 * ZIP or folder is imported, anything else goes to the SD folder). */
void a52_window_load_media(A52Window *self, const char *path);

/* Import BIOS: kind 0 files (and ZIPs), 1 one ZIP, 2 a folder. slot >= 0
 * fills that slot only (the BIOS page's per-slot Import). A dialog says what
 * was imported and what was ignored; done (may be NULL) runs afterwards. */
void a52_window_import_bios(A52Window *self, int kind, int slot,
                            void (*done)(gpointer), gpointer done_data);

/* The controller port a mouse drag in the picture steers (0-3). */
void a52_window_set_stick_port(A52Window *self, int port);

/* The view settings (aspect, smooth) from the store. */
void a52_window_apply_view(A52Window *self);

/* After Preferences: the menus and the status follow what changed. */
void a52_window_settings_changed(A52Window *self);

/* The accent colour (A52SESSION_ACCENT_RGB) as CSS classes every window
 * can use: ".a52-accent" fills a widget's background with it (white text),
 * ".a52-accent-text" colours a label (light themes only: on dark themes it
 * falls back to the theme's accent), ".a52-dot" with "-on"/"-off" a status
 * light, ".a52-held" a pressed on-screen key. Installed by main.c. */
void a52_install_accent_css(void);
