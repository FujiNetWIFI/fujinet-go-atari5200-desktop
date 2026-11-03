/*
 * Qt key events to the 5200.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QKeyEvent>

#include "a52session.h"

/* The physical key (from the native scan code: evdev + 8 on Linux) as the
 * session names it, or 0. */
uint32_t a52KeysymFromQt(const QKeyEvent *e);

/* Sends a press (down) or release to the console: true when it took it. The
 * app's own keys -- F9, F11, F12, Shift+F5, CTRL+SHIFT and ALT shortcuts --
 * are not sent (they are application-wide QActions). Auto-repeats are swallowed: the Atari repeats on its own.
 * Every window that takes typing (the main window, the Controllers panel)
 * forwards its key presses and releases through this. */
bool a52ForwardKey(a52session *session, const QKeyEvent *e, bool down);

/* A key bound to a system action (Power Cycle, Reboot to CONFIG, Pause,
 * Warp): its a52_sysaction, else -1. */
int a52KeySysaction(a52session *session, const QKeyEvent *e);
