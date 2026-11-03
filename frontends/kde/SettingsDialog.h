/*
 * The Preferences dialog, built from the session's preference table.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QDialog>
#include <functional>

#include "a52session.h"

namespace SettingsDialog {
/* Runs the dialog. Live rows apply at once (onLive runs after each, for the
 * view); machine and host rows apply together when it closes. Returns the
 * a52_apply that closing needed. */
int run(QWidget *parent, a52session *session, const std::function<void()> &onLive);
/* Import BIOS: mode 0 files, 1 a ZIP, 2 a folder; says what came of it. */
void importBios(QWidget *parent, a52session *session, int mode);
}
