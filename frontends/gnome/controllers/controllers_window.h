/*
 * The Controllers window (F9): a skinned CX52 per controller port (four, or
 * two with the 2-port BIOS) with its analog stick, fire buttons, START /
 * PAUSE / RESET and keypad, Map mode, and the gamepads with their
 * assignments. Hidden at start; a fixed-size floating window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "a52session.h"

G_BEGIN_DECLS

/* Shows (or raises) the window. */
void a52_controllers_show(GtkWindow *parent, a52session *session);
/* Shows the panel, or hides it when it is showing. parent is the
 * A52Window (keys typed with the panel active go through
 * a52_window_key). */
void a52_controllers_toggle(GtkWindow *parent, a52session *session);
gboolean a52_controllers_visible(void);
gboolean a52_controllers_active(void);
/* Bindings or settings changed outside the panel: redraw it. */
void a52_controllers_refresh(void);

G_END_DECLS
