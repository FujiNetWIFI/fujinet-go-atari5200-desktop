/*
 * File choosers: cartridges, FujiNet SD imports and the console BIOS --
 * see import.c.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef A52_WIN_IMPORT_H
#define A52_WIN_IMPORT_H

#include <windows.h>

#include "a52session.h"

/* One cartridge (.a52 .bin .rom .car, or a .zip of one) for Open; a BIOS
 * file is accepted too (the session routes it). 1 if one was chosen. */
int a52_pick_cart(HWND owner, const char *title, char *path, DWORD pathsz);

/* Asks for cartridge images (several) and copies them onto FujiNet's SD
 * host, then says so in one message box. Returns how many were copied, or
 * -1 when the chooser was cancelled. */
int a52_import_carts(HWND owner, a52session *session);

/* Import BIOS: how the user hands over the images. */
enum { A52_BIOS_FILES = 0, A52_BIOS_ZIP, A52_BIOS_FOLDER };

/* Chooses files (several), one ZIP or a folder, imports every recognised
 * BIOS image (slot < 0), or the one slot's image (slot >= 0, a file or ZIP),
 * and shows what was imported and ignored in a message box. Returns the
 * number of images imported, 0 when none, -1 when the chooser was cancelled. */
int a52_import_bios(HWND owner, a52session *session, int how, int slot);

/* A path (a drop, the command line) the session turned into a BIOS import:
 * shows its message. */
void a52_show_import_message(HWND owner, int imported, const char *msg);

/* The same three choices as a pop-up menu at a screen point; returns the
 * result of a52_import_bios, or -1 if nothing was chosen. */
int a52_import_bios_menu(HWND owner, a52session *session, int slot, int x, int y);

#endif
