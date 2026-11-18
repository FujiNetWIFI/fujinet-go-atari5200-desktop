/*
 * The Win32 Controllers window (F9): the CX52 controllers drawn from the
 * session's layout, the gamepads with their assignments, the mouse's port and
 * Map mode, in a fixed-size tool window. Hidden at start.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <windows.h>

#include "a52session.h"

/* Show / hide (F9). Created on first use, hidden not destroyed after. */
void a52_controllers_window_toggle(HWND parent, a52session *session);
/* Show without toggling (Preferences' Open Controllers). */
void a52_controllers_window_show(HWND parent, a52session *session);
int  a52_controllers_window_visible(void);

/* The gamepad set, the mouse port or the console's port count changed. */
void a52_controllers_window_refresh(void);

/* Map mode is armed: the next host key is a binding, not typing. The main
 * window gives its keys here first; 1 if consumed. */
int a52_controllers_map_key(UINT msg, WPARAM vk, LPARAM lp);
