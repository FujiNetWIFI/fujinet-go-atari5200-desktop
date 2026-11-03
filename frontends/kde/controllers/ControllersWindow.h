/*
 * The Controllers panel (F9): the 5200 keypad with START / PAUSE / RESET, the
 * four analog sticks and the gamepads with their assignments and bindings. Hidden at start; a fixed-size floating window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QWidget>

#include "a52session.h"

/* Shows the panel, or hides it when it is showing. */
void a52ControllersToggle(QWidget *parent, a52session *session);
bool a52ControllersVisible();
