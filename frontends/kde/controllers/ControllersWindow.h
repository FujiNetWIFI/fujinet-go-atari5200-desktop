/*
 * The Controllers panel (F9): a CX52 controller for every port the console
 * has (four, or two with the 2-port BIOS), drawn from a52session_controls_layout,
 * the gamepads with their assignments, and Map mode for the bindings. Hidden at
 * start; a fixed-size floating window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QWidget>

#include "a52session.h"

/* Shows the panel, or hides it when it is showing. */
void a52ControllersToggle(QWidget *parent, a52session *session);
/* Shows (and raises) the panel; leaves it showing if it already is. */
void a52ControllersShow(QWidget *parent, a52session *session);
bool a52ControllersVisible();
/* The panel is the active window (the main window then keeps its keys). */
bool a52ControllersActive();
/* Settings changed (a BIOS, the key bindings): redraw the controllers. */
void a52ControllersRefresh();
