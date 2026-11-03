/*
 * Where the picture goes inside the display, shared by the GPU and the
 * software display, and the pointer mapping back from it.
 *
 * The frame's size changes with the visible area; its pixel aspect (x1000,
 * always 1000 here) says how wide each pixel is against the 5200's own. TV
 * aspect then gives the pixels their shape on a TV (NTSC 6:7, PAL a little
 * wide).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QtGlobal>

#include "a52session.h"

/* The picture's width over its height, as shown. */
inline double pictureAspect(a52session *session, int w, int h, int par, bool tv)
{
    const double pw = w * par / 1000.0;
    if (!tv)
        return pw / h;
    /* a 5200 pixel on an NTSC TV is 6/7 as wide as tall; on PAL, 1.03 */
    return pw * (a52session_refresh_rate(session) > 55000 ? 6.0 / 7.0 : 1.03) / h;
}

/* The largest rectangle of the given aspect that fits, centred. */
inline QRectF fitRect(QSizeF avail, double want)
{
    const double w = avail.width(), h = avail.height();
    double sw, sh;
    if (w / h > want) { sh = h; sw = sh * want; }
    else              { sw = w; sh = sw / want; }
    return QRectF((w - sw) / 2.0, (h - sh) / 2.0, sw, sh);
}

/* The pointer: dragging with the left button in the picture moves the analog
 * stick of the selected port ("mouse_port" setting, 0-3): the stick follows
 * the pointer's distance from where the button went down, a sixth of the
 * picture's width being full deflection. Letting go centres it. */
class PointerMapper {
public:
    explicit PointerMapper(a52session *session) : m_session(session) {}

    /* p in widget coordinates; shown is where the w x h frame is drawn. */
    void at(const QPointF &p, unsigned buttons, const QRectF &shown, int, int)
    {
        const bool down = (buttons & 1) != 0;
        if (!down) {
            release();
            return;
        }
        if (!m_down) {
            m_down = true;
            m_origin = p;
            m_port = qBound(0, a52session_get_int(m_session, "mouse_port", 0), A52_PORTS - 1);
        }
        const double radius = qMax(8.0, shown.width() / 6.0);
        const int x = qBound(-128, (int)((p.x() - m_origin.x()) * 127.0 / radius), 127);
        const int y = qBound(-128, (int)((p.y() - m_origin.y()) * 127.0 / radius), 127);
        a52session_stick_set(m_session, m_port, x, y);
    }

    void leave() {}   /* a drag goes on outside the picture; the release ends it */

private:
    void release()
    {
        if (!m_down)
            return;
        m_down = false;
        a52session_stick_set(m_session, m_port, 0, 0);
    }

    a52session *m_session;
    QPointF m_origin;
    int m_port = 0;
    bool m_down = false;
};
