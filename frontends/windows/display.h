/*
 * The emulator picture -- see display.c.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef A52_WIN_DISPLAY_H
#define A52_WIN_DISPLAY_H

#include <windows.h>

#include "a52session.h"

/* Creates the picture as a child of parent (size it with MoveWindow) and
 * starts its render thread. One per process. */
HWND a52_display_create(HWND parent, a52session *session);
/* Stops the render thread; call before the parent is destroyed. */
void a52_display_destroy(void);
/* The picture is on screen (the main window is not minimized): off, the
 * render thread sleeps until it is on again, with no wakeups at all. */
void a52_display_set_active(int active);
/* tv: the television's 4:3 (else square pixels, 192:262); smooth: bilinear
 * (else sharp pixels). Safe from any thread. */
void a52_display_set_options(int tv, int smooth);
/* "Direct3D 11", "Direct3D 11 (WARP)" or "GDI": what is drawing. */
const char *a52_display_renderer(void);

#endif
