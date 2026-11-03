/*
 * The debugger window (F12).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QWidget>

#include "a52session.h"

/* Shows the debugger (attaching it stops the machine), or hides it
 * (detaching lets it run). */
void a52DebuggerToggle(QWidget *parent, a52session *session);
void a52DebuggerShow(QWidget *parent, a52session *session);
