/*
 * AppKit key events to the Atari 5200: the hardware key code through the
 * session's table (so a press and its release pair up), with the character
 * the key typed for the main-row keys Shift changes (* and #).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>
#include <stdint.h>

#include "a52session.h"

/* The keysym for a key event (the session's X11-style keysym), or 0. */
uint32_t A52KeysymFromEvent(NSEvent *event);

/* YES for the app's own keys: F6 (Help), F9, F11, F12 and Shift+F5. The
 * caller handles them before offering a key to the console. */
BOOL A52IsAppKey(NSEvent *event);

/* A keyDown/keyUp for the console: YES when it took it. Command shortcuts
 * are the menus'; the app's keys are the caller's. A release always lets go
 * of whatever its press pressed (Shift may have changed meanwhile).
 * Auto-repeats are swallowed. */
BOOL A52ForwardKey(a52session *session, NSEvent *event, BOOL down);

/* flagsChanged: Shift, Control and Option as bindable keys. */
void A52ForwardFlags(a52session *session, NSEvent *event);

/* Lets go of every key this module has pressed (focus lost). */
void A52ForwardReleaseAll(a52session *session);

/* This app's accent (A52SESSION_ACCENT_RGB, the console's POWER orange): a
 * fill, with dark text on it. As text on a light appearance it is too
 * light: A52AccentTextColor() gives the dark variant there. */
NSColor *A52AccentColor(void);
NSColor *A52AccentTextColor(NSAppearance *appearance);
