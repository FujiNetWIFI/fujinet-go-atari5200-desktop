/*
 * The Controllers panel (F9): the 5200 keypad with START / PAUSE / RESET,
 * the four controller ports with their analog sticks and fire buttons, and
 * the gamepads with their assignments and bindings. Hidden at start; a
 * fixed-size floating window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "a52session.h"

G_BEGIN_DECLS

/* Shows the panel, or hides it when it is showing. parent is the
 * A52Window (keys typed with the panel active go through
 * a52_window_key). */
void a52_controllers_toggle(GtkWindow *parent, a52session *session);
gboolean a52_controllers_visible(void);
/* Bindings or settings changed outside the panel: redraw it. */
void a52_controllers_refresh(void);

G_END_DECLS
