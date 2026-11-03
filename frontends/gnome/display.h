/*
 * The Atari's picture: a GtkWidget that pulls frames from the session on
 * the compositor's frame clock.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "a52session.h"

#define A52_TYPE_DISPLAY (a52_display_get_type())
G_DECLARE_FINAL_TYPE(A52Display, a52_display, A52, DISPLAY, GtkWidget)

GtkWidget *a52_display_new(a52session *session);
/* The TV's shape (the frame's pixel aspect applied), or square pixels. */
void a52_display_set_tv_aspect(A52Display *self, gboolean tv);
void a52_display_set_smooth(A52Display *self, gboolean smooth);
/* The controller port a mouse drag in the picture steers (0-3). */
void a52_display_set_stick_port(A52Display *self, int port);
/* Saves the last frame as a PNG; FALSE on failure. */
gboolean a52_display_save_png(A52Display *self, const char *path);
/* Frames the machine has run, once a picture has reached the widget
 * (A52_AUTOQUIT_FRAMES). */
guint64 a52_display_frames(A52Display *self);
