/*
 * The FujiNet console log window, the FujiNet web UI, and the accent.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QColor>
#include <QWidget>

#include "a52session.h"

/* Shows (raising an existing one) the FujiNet console log window. */
void fujinet_log_show(QWidget *parent, a52session *session);
/* Opens the FujiNet web UI in the system browser. Not embedded: the web UI's
 * Google and OneDrive Authorize buttons open the provider's consent page in a
 * new tab, and both providers refuse OAuth from an embedded web view. */
void fujinet_config_show(QWidget *parent, a52session *session);

/* The accent (A52SESSION_ACCENT_RGB, the 5200's POWER orange): a fill colour, with
 * white text on it. As a text colour on a dark palette it is too dark:
 * a52AccentTextColor() gives a lightened orange there. */
QColor a52AccentColor();
QColor a52AccentTextColor(const QWidget *w);
