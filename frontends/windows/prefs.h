/*
 * The Win32 Settings window, built from the session's preference table
 * (a52session_pref) like the other frontends' Preferences.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <windows.h>

#include "a52session.h"

/* Shows the window (raising it when open). Live rows apply at once and
 * call on_live (the view follows); console and host rows apply together
 * when it closes, which calls on_closed with the a52_apply it needed. */
void a52_prefs_show(HWND parent, a52session *session, void (*on_live)(void),
                     void (*on_closed)(int applied));
/* Keyboard navigation for the window, from the message loop: 1 if taken. */
int a52_prefs_pretranslate(MSG *msg);
/* The main window is closing: closes Preferences without applying. */
void a52_prefs_abandon(void);
