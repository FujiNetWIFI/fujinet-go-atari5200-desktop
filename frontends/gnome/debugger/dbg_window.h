/*
 * The debugger window (F12).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "a52session.h"

G_BEGIN_DECLS

/* Shows the debugger (attaching it stops the machine), or hides it
 * (detaching lets it run). */
void a52_debugger_toggle(GtkWindow *parent, a52session *session);
void a52_debugger_show(GtkWindow *parent, a52session *session);

G_END_DECLS
