/*
 * The Preferences dialog.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "a52session.h"
#include "window.h"

G_BEGIN_DECLS

/* Shows the Preferences (built from a52session_pref). Live rows apply at
 * once; machine and host rows apply together when it closes. */
void a52_prefs_show(A52Window *parent, a52session *session);

G_END_DECLS
