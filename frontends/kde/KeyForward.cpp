/*
 * KeyForward -- see KeyForward.h.
 *
 * Keys are keyed by the physical key so a press and its release always
 * pair up, whatever the modifiers did in between; the character Qt says
 * the key typed goes with the press.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KeyForward.h"

uint32_t a52KeysymFromQt(const QKeyEvent *e)
{
    const quint32 code = e->nativeScanCode();
    uint32_t ks = code >= 8 ? a52session_keysym_from_evdev(code - 8) : 0;
    if (!ks) {
        const int k = e->key();
        if (k >= Qt::Key_A && k <= Qt::Key_Z)
            ks = (uint32_t)('a' + (k - Qt::Key_A));
        else if (k >= 0x20 && k < 0x7f)
            ks = (uint32_t)k;
    }
    return ks;
}

static bool appKey(const QKeyEvent *e)
{
    const Qt::KeyboardModifiers m = e->modifiers();
    const int k = e->key();
    if (k == Qt::Key_F9 || k == Qt::Key_F11 || k == Qt::Key_F12)
        return true;
    if (k == Qt::Key_F5 && (m & Qt::ShiftModifier))
        return true;
    if (m & (Qt::AltModifier | Qt::MetaModifier))
        return true;
    /* CTRL+SHIFT is the app's, except the modifier keys themselves */
    if ((m & Qt::ControlModifier) && (m & Qt::ShiftModifier) && k != Qt::Key_Shift &&
        k != Qt::Key_Control)
        return true;
    return (m & Qt::ControlModifier) && k == Qt::Key_Comma;
}

bool a52ForwardKey(a52session *session, const QKeyEvent *e, bool down)
{
    if (e->isAutoRepeat())
        return !appKey(e);
    const uint32_t ks = a52KeysymFromQt(e);
    if (!down) {
        /* always: a key let go while a shortcut was held must not stick */
        a52session_key(session, ks, 0);
        return ks != 0 && !appKey(e);
    }
    if (appKey(e) || !ks)
        return false;
    return a52session_key(session, ks, 1) != 0;
}

int a52KeySysaction(a52session *session, const QKeyEvent *e)
{
    if (e->isAutoRepeat() || appKey(e))
        return -1;
    const uint32_t ks = a52KeysymFromQt(e);
    return ks ? a52session_key_sysaction(session, ks) : -1;
}
