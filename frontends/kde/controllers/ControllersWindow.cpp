/*
 * ControllersWindow -- see ControllersWindow.h.
 *
 * One painted widget over the session's layout (a52session_controls_layout, in
 * layout units; a52session_controls_hit says what a click landed on), so every
 * frontend shows the same parts in the same places: a CX52 for each port the
 * console has -- a long black wedge, the three console keys along the top, the
 * analog stick in its dish, two pairs of red side buttons, a silver band and
 * the recessed 12-key pad -- side by side, and a line under them saying the
 * keypad and FIRE 2 are shared. The colours are the A52SESSION_*_RGB defines,
 * read off photographs of the real thing (controllers-port/DESIGN-5200.md).
 *
 * Gestures act on mouse press AND release, not "clicked": the console scans
 * its inputs once a frame, so a press present only for the instant of a click
 * would fall between frames. A side button or a key is HELD while the mouse is
 * down on it (dragging off lets go, back on holds again); dragging the stick
 * is analog (a52session_stick_set from the distance to the dish's centre) and
 * releasing centres it; the wheel nudges it for a moment. Everything the mouse
 * holds is let go on release, on losing the focus, on hiding and on entering
 * Map.
 *
 * What the console sees held -- host keyboard, gamepads and this window -- is
 * lit with the HELD ring and a fill, so the window doubles as an input tester
 * for a new pad. The keypad and FIRE 2 are one wire for all controllers, so a
 * key held on any of them lights on all. Keys typed with the window active go
 * to the console as in the main window.
 *
 * FIXED SIZE: the drawing is a fixed number of pixels per layout unit (the
 * window follows the number of ports), Map legends are fitted into their
 * parts and gamepad names are elided, so neither Map mode nor a long name
 * resizes it. A singleton, hidden rather than destroyed, so a half-done
 * remap survives closing it. The timers run only while it is visible.
 *
 * MAP MODE: Map arms a two-step rebind -- click a part (a side button, a key,
 * a console key, or a side of the stick's dish), then press a host key or a
 * gamepad button. Rebinding steals the key from whatever held it (named in
 * the hint). Escape cancels; while Map is armed nothing is typed into the
 * console. F9, F11 and F12 cannot be bound.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ControllersWindow.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPolygonF>
#include <QPushButton>
#include <QRadialGradient>
#include <QRadioButton>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtMath>

#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

#include "../FujiNetWindows.h"
#include "../KeyForward.h"
#include "../help/HelpWindow.h"

namespace {

constexpr int kMaxPadRows = 3;
constexpr qreal kDimAlpha = 0.4;        /* an unpicked part while Map waits for a key */
constexpr qreal kUnit = 12.0;           /* pixels per layout unit */
constexpr int kMargin = 12;             /* round the drawing */
constexpr int kMouseStrip = 26;         /* the "Mouse" selector under the note */
constexpr int kNudge = 90;              /* one wheel notch of the stick, of 127 */
constexpr int kNudgeMs = 350;           /* ... held this long */

QColor rgb(unsigned value) { return QColor((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF); }

QString local(const char *text) { return QString::fromUtf8(text ? text : ""); }

QFont pixelFont(QFont fnt, qreal pixels, bool bold, bool italic = false)
{
    fnt.setPixelSize(qMax(1, qRound(pixels)));
    fnt.setBold(bold);
    fnt.setItalic(italic);
    return fnt;
}

/* Lines centred in rect, the font shrunk until they fit. */
void drawFitted(QPainter &pnt, const QRectF &rect, const QString &text, QFont fnt)
{
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (;;) {
        const QFontMetricsF metrics(fnt);
        qreal wide = 0;
        for (const QString &line : lines) wide = qMax(wide, metrics.horizontalAdvance(line));
        const int size = fnt.pixelSize();
        if ((wide <= rect.width() && metrics.lineSpacing() * lines.size() <= rect.height() + 2) || size <= 5)
            break;
        fnt.setPixelSize(size - 1);
    }
    pnt.setFont(fnt);
    pnt.drawText(rect, Qt::AlignCenter, text);
}

/* Long key names shortened to fit a key (tooltips keep the full name). */
QString shorten(QString text)
{
    text.replace(QStringLiteral("Keypad "), QStringLiteral("KP "));
    text.replace(QStringLiteral("Left "), QStringLiteral("L "));
    text.replace(QStringLiteral("Right "), QStringLiteral("R "));
    text.replace(QStringLiteral("Backspace"), QStringLiteral("BkSp"));
    text.replace(QStringLiteral("Escape"), QStringLiteral("Esc"));
    text.replace(QStringLiteral("Return"), QStringLiteral("Ret"));
    text.replace(QStringLiteral("Enter"), QStringLiteral("Ent"));
    return text;
}

/* A target's key and pad button, as Map and Show bindings write them. */
QString keyText(a52session *s, int target)
{
    const a52_binding bnd = a52session_binding_get(s, target);
    char key[48] = "";
    if (bnd.keysym) a52session_keysym_name(bnd.keysym, key, sizeof key);
    return local(key);
}

QString padText(a52session *s, int target)
{
    const a52_binding bnd = a52session_binding_get(s, target);
    return bnd.button == A52_PAD_BTN_NONE ? QString() : local(a52_pad_button_name(bnd.button));
}

/* ---- ElidedLabel ------------------------------------------------------------ */

/* A label that never widens the window: one line elides, more wrap into a
 * fixed height. */
class ElidedLabel : public QWidget {
public:
    explicit ElidedLabel(bool dim, int lines = 1) : m_dim(dim), m_lines(lines)
    {
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    }
    void setText(const QString &text)
    {
        if (text == m_text) return;
        m_text = text;
        update();
    }
    QString text() const { return m_text; }
    QSize sizeHint() const override { return QSize(0, fontMetrics().lineSpacing() * m_lines); }
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter pnt(this);
        pnt.setPen(m_dim ? QColor(Qt::gray) : palette().color(QPalette::WindowText));
        if (m_lines > 1)
            pnt.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap, m_text);
        else
            pnt.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter,
                         fontMetrics().elidedText(m_text, Qt::ElideRight, width()));
    }

private:
    QString m_text;
    bool m_dim;
    int m_lines;
};

/* ---- StageWidget: the controllers --------------------------------------------- */

class StageWidget : public QWidget {
public:
    /* onPick: a part was chosen in Map mode (its target) */
    StageWidget(a52session *session, std::function<void(int)> onPick)
        : m_session(session), m_onPick(std::move(onPick))
    {
        setFocusPolicy(Qt::NoFocus);
        setMouseTracking(true);
        m_nudge.setSingleShot(true);
        connect(&m_nudge, &QTimer::timeout, this, [this] {
            if (m_hold == None) a52session_release_pointer(m_session);
            pollHeld();
        });
        /* "Mouse" under each controller: which one the picture's mouse mode drives */
        m_mouseGroup = new QButtonGroup(this);
        m_mouseGroup->setExclusive(true);
        for (int port = 0; port < A52_PORTS; ++port) {
            auto *radio = new QRadioButton(QStringLiteral("Mouse"), this);
            radio->setFocusPolicy(Qt::NoFocus);
            radio->setToolTip(QStringLiteral("The picture's mouse mode drives this controller's stick and FIRE"));
            m_mouseGroup->addButton(radio, port);
            m_radios[port] = radio;
        }
        connect(m_mouseGroup, &QButtonGroup::idClicked, this,
                [this](int port) { a52session_set_int(m_session, "mouse_port", port); });
        load();
    }

    /* Rebuilds the layout when the console's port count changed. True if it did. */
    bool reload()
    {
        if (a52session_active_ports(m_session) == m_ports) return false;
        releaseAll();
        load();
        return true;
    }

    /* Lets go of everything the mouse holds (also on focus loss, Map entry and hide). */
    void releaseAll()
    {
        m_nudge.stop();
        a52session_release_pointer(m_session);
        m_hold = None;
        m_holdIdx = -1;
        pollHeld();
        update();
    }

    /* -2 idle, -1 armed, >= 0 waiting for a key or pad button for that target */
    void setMap(int state)
    {
        m_map = state;
        update();
    }
    void setShowBindings(bool visible)
    {
        m_showBindings = visible;
        update();
    }

    /* What the console sees held (keyboard, gamepads, the mouse), the sticks'
     * deflections and the mouse-port selector; repaints only on a change. */
    void pollHeld()
    {
        bool changed = false;
        for (int idx = 0; idx < m_count; ++idx) {
            const a52_control &ctl = m_lay[(size_t)idx];
            char held = 0;
            if (ctl.kind == A52_CTL_STICK) {
                int xval = 0, yval = 0;
                a52session_stick_get(m_session, ctl.port, &xval, &yval);
                held = (char)(xval || yval ? 1 : 0);
                if (m_stick[ctl.port][0] != xval || m_stick[ctl.port][1] != yval) {
                    m_stick[ctl.port][0] = xval;
                    m_stick[ctl.port][1] = yval;
                    changed = true;
                }
            } else if (ctl.kind == A52_CTL_FIRE || ctl.kind == A52_CTL_CONSOLE_KEY || ctl.kind == A52_CTL_KEYCAP) {
                held = a52session_target_held(m_session, ctl.target) ? 1 : 0;
            } else {
                continue;
            }
            if (held != m_held[(size_t)idx]) {
                m_held[(size_t)idx] = held;
                changed = true;
            }
        }
        const int mouse = qBound(0, a52session_get_int(m_session, "mouse_port", 0), A52_PORTS - 1);
        if (!m_radios[mouse]->isChecked()) m_radios[mouse]->setChecked(true);
        if (changed) update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter pnt(this);
        pnt.setRenderHint(QPainter::Antialiasing);
        pnt.setRenderHint(QPainter::TextAntialiasing);
        /* opaque: Qt's Format_RGB32 grabs carry no alpha (the KDE transparency trap) */
        pnt.fillRect(rect(), palette().color(QPalette::Window));
        for (int idx = 0; idx < m_count; ++idx) {
            const a52_control &ctl = m_lay[(size_t)idx];
            switch (ctl.kind) {
            case A52_CTL_CABLE: paintCable(pnt, idx); break;
            case A52_CTL_BODY: paintBody(pnt, idx); break;
            case A52_CTL_BAND: paintBand(pnt, idx); break;
            case A52_CTL_CAPTION: paintCaption(pnt, idx); break;
            case A52_CTL_CONSOLE_KEY: paintConsoleKey(pnt, idx); break;
            case A52_CTL_DISH: paintDish(pnt, idx); break;
            case A52_CTL_FIRE: paintFire(pnt, idx); break;
            case A52_CTL_WELL: paintWell(pnt, idx); break;
            case A52_CTL_KEYCAP: paintKeycap(pnt, idx); break;
            case A52_CTL_SLOT: paintSlot(pnt, idx); break;
            case A52_CTL_NOTE: paintNote(pnt, idx); break;
            default: break;      /* STICK: drawn with its dish */
            }
        }
    }

    void mousePressEvent(QMouseEvent *evt) override
    {
        if (evt->button() != Qt::LeftButton) return;
        if (m_hold != None) releaseAll();     /* a stale hold: its release was lost */
        const QPointF pos = evt->position();
        const int idx = hitAt(pos);
        if (m_map != -2) {
            if (idx < 0) return;
            const a52_control &ctl = m_lay[(size_t)idx];
            int target = ctl.target;
            if (ctl.kind == A52_CTL_STICK) {
                const QPointF mid = centreOf(idx);
                target = a52session_stick_map_target(ctl.port, float(pos.x() - mid.x()), float(pos.y() - mid.y()),
                                                     float(radiusOf(idx)));
            }
            if (target >= 0) m_onPick(target);
            return;
        }
        if (idx < 0) return;
        const a52_control &ctl = m_lay[(size_t)idx];
        m_nudge.stop();
        m_holdIdx = idx;
        m_partDown = true;
        if (ctl.kind == A52_CTL_STICK) {
            m_hold = Stick;
            driveStick(pos);
        } else {
            m_hold = Part;
            a52session_press(m_session, ctl.target, 1);
        }
        pollHeld();
        update();
    }

    /* Dragging off a part lets it go (and back on, holds it again). */
    void mouseMoveEvent(QMouseEvent *evt) override
    {
        if (m_hold != None && !(evt->buttons() & Qt::LeftButton)) {
            releaseAll();       /* the release never came (a grab lost) */
            return;
        }
        const QPointF pos = evt->position();
        const int hover = m_hold == None ? hitAt(pos) : -1;
        if (hover != m_hover) {
            m_hover = hover;
            update();
        }
        if (m_hold == Stick) {
            driveStick(pos);
            pollHeld();
        } else if (m_hold == Part) {
            const bool over = hitAt(pos) == m_holdIdx;
            if (over != m_partDown) {
                m_partDown = over;
                a52session_press(m_session, m_lay[(size_t)m_holdIdx].target, over ? 1 : 0);
                pollHeld();
                update();
            }
        }
    }

    void mouseReleaseEvent(QMouseEvent *evt) override
    {
        if (evt->button() != Qt::LeftButton) return;
        if (m_hold != None) releaseAll();
        m_hover = hitAt(evt->position());
        update();
    }

    /* The wheel nudges the stick under the pointer for a moment: up and down
     * with the wheel, left and right with a sideways one. */
    void wheelEvent(QWheelEvent *evt) override
    {
        if (m_map != -2 || m_hold != None) return;
        const int idx = hitAt(evt->position());
        if (idx < 0 || m_lay[(size_t)idx].kind != A52_CTL_STICK) return;
        const int xnotch = evt->angleDelta().x(), ynotch = evt->angleDelta().y();
        const int xval = xnotch > 0 ? kNudge : xnotch < 0 ? -kNudge : 0;
        const int yval = ynotch > 0 ? -kNudge : ynotch < 0 ? kNudge : 0;     /* wheel up = stick up */
        if (!xval && !yval) return;
        a52session_stick_set(m_session, m_lay[(size_t)idx].port, xval, yval);
        m_nudge.start(kNudgeMs);
        pollHeld();
        evt->accept();
    }

    void leaveEvent(QEvent *evt) override
    {
        if (m_hover != -1) {
            m_hover = -1;
            update();
        }
        QWidget::leaveEvent(evt);
    }

    bool event(QEvent *evt) override
    {
        if (evt->type() == QEvent::ToolTip) {
            auto *help = static_cast<QHelpEvent *>(evt);
            const QString tip = tipAt(help->pos());
            if (tip.isEmpty())
                QToolTip::hideText();
            else
                QToolTip::showText(help->globalPos(), tip, this);
            return true;
        }
        return QWidget::event(evt);
    }

private:
    enum Hold { None, Part, Stick };

    void load()
    {
        m_ports = a52session_active_ports(m_session);
        std::vector<a52_control> lay((size_t)A52_CONTROLS_MAX);
        float wide = 0, tall = 0;
        const int count = a52session_controls_layout(m_ports, lay.data(), A52_CONTROLS_MAX, &wide, &tall);
        lay.resize((size_t)count);
        m_lay = std::move(lay);
        m_count = count;
        m_held.assign((size_t)m_count, 0);
        for (auto &pair : m_stick) pair[0] = pair[1] = 0;
        const int stageW = qCeil(wide * kUnit) + 2 * kMargin;
        const int stageH = qCeil(tall * kUnit) + 2 * kMargin;
        setFixedSize(stageW, stageH + kMouseStrip);
        /* the selector sits under each controller, centred on its cell */
        for (int port = 0; port < A52_PORTS; ++port) {
            m_radios[port]->setVisible(port < m_ports);
            if (port >= m_ports) continue;
            const QSize size = m_radios[port]->sizeHint();
            const qreal mid = kMargin + (0.7 + 6.0 + port * 14.4) * kUnit;
            m_radios[port]->setGeometry(qRound(mid - size.width() / 2.0), stageH + (kMouseStrip - size.height()) / 2,
                                        size.width(), size.height());
        }
        update();
    }

    QRectF rectOf(int idx) const
    {
        const a52_control &ctl = m_lay[(size_t)idx];
        return QRectF(kMargin + ctl.x * kUnit, kMargin + ctl.y * kUnit, ctl.w * kUnit, ctl.h * kUnit);
    }
    QPointF centreOf(int idx) const { return rectOf(idx).center(); }
    qreal radiusOf(int idx) const { return rectOf(idx).width() / 2; }

    int hitAt(const QPointF &pos) const
    {
        return a52session_controls_hit(m_lay.data(), m_count, float((pos.x() - kMargin) / kUnit),
                                       float((pos.y() - kMargin) / kUnit));
    }

    /* The mouse is dragging the stick of m_holdIdx: x,y from the distance to
     * the dish's centre, full at the drag region's edge. */
    void driveStick(const QPointF &pos)
    {
        const a52_control &ctl = m_lay[(size_t)m_holdIdx];
        const QPointF mid = centreOf(m_holdIdx);
        int xval, yval;
        a52session_stick_from_vector(float(pos.x() - mid.x()), float(pos.y() - mid.y()),
                                     float(radiusOf(m_holdIdx)), &xval, &yval);
        a52session_stick_set(m_session, ctl.port, xval, yval);
    }

    QString tipAt(const QPoint &pos) const
    {
        const int idx = hitAt(pos);
        if (idx < 0) return QString();
        const a52_control &ctl = m_lay[(size_t)idx];
        if (ctl.kind == A52_CTL_STICK) {
            QStringList lines;
            lines << QStringLiteral("Controller %1 stick: drag for analog, wheel nudges").arg(ctl.port + 1);
            for (int act = A52_ACT_UP; act <= A52_ACT_RIGHT; ++act) {
                const int target = A52_TARGET_PORT(ctl.port, act);
                lines << QStringLiteral("%1: %2").arg(local(a52_target_short_name(target)), bound(target));
            }
            return lines.join(QLatin1Char('\n'));
        }
        return QStringLiteral("%1\nBound to: %2").arg(local(a52_target_name(ctl.target)), bound(ctl.target));
    }

    QString bound(int target) const
    {
        const QString key = keyText(m_session, target), pad = padText(m_session, target);
        QString text = key;
        if (!pad.isEmpty()) text += (text.isEmpty() ? QString() : QStringLiteral(" / ")) + pad;
        return text.isEmpty() ? QStringLiteral("nothing") : text;
    }

    bool mapping() const { return m_map != -2; }
    bool showBind() const { return mapping() || m_showBindings; }

    /* A part's lit state: held by anyone, or the one Map picked. */
    bool lit(int idx) const
    {
        const a52_control &ctl = m_lay[(size_t)idx];
        if (mapping()) return ctl.target >= 0 && ctl.target == m_map;
        return m_held[(size_t)idx] != 0;
    }

    /* While Map waits for a key, every part but the picked one steps back. */
    bool dimmed(const a52_control &ctl) const
    {
        if (m_map < 0) return false;
        if (ctl.kind == A52_CTL_STICK) return m_map / A52_ACT_PER_PORT != ctl.port || m_map % A52_ACT_PER_PORT > A52_ACT_RIGHT;
        return ctl.target != m_map;
    }

    /* The held look: a ring in the HELD colour and a 35% fill of it. */
    void held(QPainter &pnt, const QPainterPath &shape)
    {
        QColor fill = rgb(A52SESSION_HELD_RGB);
        fill.setAlphaF(0.35);
        pnt.setPen(QPen(rgb(A52SESSION_HELD_RGB), 2));
        pnt.setBrush(fill);
        pnt.drawPath(shape);
    }

    /* The body's outline: 12 u at the top, A52_BODY_BOTTOM_W at the keypad end. */
    QPainterPath bodyPath(const QRectF &box) const
    {
        const qreal inset = box.width() * (1.0 - A52_BODY_BOTTOM_W / 12.0) / 2;
        QPolygonF poly;
        poly << box.topLeft() << box.topRight() << QPointF(box.right() - inset, box.bottom())
             << QPointF(box.left() + inset, box.bottom());
        QPainterPath path;
        path.addPolygon(poly);
        path.closeSubpath();
        /* soften the corners a little */
        QPainterPath round;
        round.addRoundedRect(box, 6, 6);
        return path.intersected(round);
    }

    int bodyIndex(int port) const
    {
        for (int idx = 0; idx < m_count; ++idx)
            if (m_lay[(size_t)idx].kind == A52_CTL_BODY && m_lay[(size_t)idx].port == port) return idx;
        return -1;
    }

    /* ---- painting ---- */

    void paintCable(QPainter &pnt, int idx)
    {
        const QRectF box = rectOf(idx);
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(rgb(A52SESSION_CASE_SHADOW_RGB));
        pnt.drawRoundedRect(box.adjusted(2, 0, -2, 2), 2, 2);
    }

    void paintBody(QPainter &pnt, int idx)
    {
        const QRectF box = rectOf(idx);
        const QPainterPath shape = bodyPath(box);
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(rgb(A52SESSION_CASE_SHADOW_RGB));
        pnt.drawPath(shape.translated(0, 2));
        QLinearGradient grad(box.topLeft(), box.topRight());
        grad.setColorAt(0, rgb(A52SESSION_CASE_RGB).lighter(112));
        grad.setColorAt(0.5, rgb(A52SESSION_CASE_RGB));
        grad.setColorAt(1, rgb(A52SESSION_CASE_RGB).darker(110));
        pnt.setBrush(grad);
        pnt.setPen(QPen(rgb(A52SESSION_CASE_BEVEL_RGB), 1.5));
        pnt.drawPath(shape);
    }

    void paintBand(QPainter &pnt, int idx)
    {
        const QRectF box = rectOf(idx);
        const int body = bodyIndex(m_lay[(size_t)idx].port);
        pnt.save();
        if (body >= 0) pnt.setClipPath(bodyPath(rectOf(body)));
        QLinearGradient grad(box.topLeft(), box.bottomLeft());
        grad.setColorAt(0, rgb(A52SESSION_BAND_HI_RGB));
        grad.setColorAt(1, rgb(A52SESSION_BAND_LO_RGB));
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(grad);
        pnt.drawRect(box);
        /* the rainbow line under the text */
        const qreal line = box.top() + box.height() * 0.80, thick = box.height() * 0.11;
        const QRectF text = bandTextRect(idx);
        const unsigned colours[4] = { A52SESSION_RAINBOW_ORANGE_RGB, A52SESSION_RAINBOW_YELLOW_RGB,
                                      A52SESSION_RAINBOW_BLUE_RGB, A52SESSION_RAINBOW_GREEN_RGB };
        for (int band = 0; band < 4; ++band) {
            pnt.setBrush(rgb(colours[band]));
            pnt.drawRect(QRectF(text.left(), line + band * thick * 0.9, text.width(), thick));
        }
        /* the embossed fuji mark at the left: three slanting bars */
        const qreal mx = box.left() + 1.1 * kUnit, my = box.top() + box.height() * 0.22,
                    mh = box.height() * 0.48;
        pnt.setBrush(rgb(A52SESSION_BAND_TEXT_RGB));
        for (int bar = 0; bar < 3; ++bar) {
            const qreal bx = mx + bar * 0.42 * kUnit;
            QPolygonF poly;
            poly << QPointF(bx, my + mh) << QPointF(bx + 0.20 * kUnit, my) << QPointF(bx + 0.34 * kUnit, my)
                 << QPointF(bx + 0.14 * kUnit, my + mh);
            pnt.drawPolygon(poly);
        }
        pnt.restore();
    }

    QRectF bandTextRect(int bandIdx) const
    {
        for (int idx = 0; idx < m_count; ++idx)
            if (m_lay[(size_t)idx].kind == A52_CTL_CAPTION && m_lay[(size_t)idx].port == m_lay[(size_t)bandIdx].port
                && m_lay[(size_t)idx].y < m_lay[(size_t)bandIdx].y + m_lay[(size_t)bandIdx].h)
                return rectOf(idx);
        return rectOf(bandIdx);
    }

    void paintCaption(QPainter &pnt, int idx)
    {
        const a52_control &ctl = m_lay[(size_t)idx];
        const QRectF box = rectOf(idx);
        const QString text = local(ctl.label);
        if (text == QLatin1String("ATARI 5200")) {
            pnt.setPen(rgb(A52SESSION_BAND_TEXT_RGB));
            QFont fnt = pixelFont(font(), 11, true);
            fnt.setLetterSpacing(QFont::AbsoluteSpacing, 0.8);
            drawFitted(pnt, box, text, fnt);
        } else {                                   /* "Controller N", under the body */
            pnt.setPen(palette().color(QPalette::WindowText));
            drawFitted(pnt, box.adjusted(0, -1, 0, 3), text, pixelFont(font(), 12, true));
        }
    }

    void paintNote(QPainter &pnt, int idx)
    {
        pnt.setPen(QColor(Qt::gray));
        drawFitted(pnt, rectOf(idx).adjusted(0, 0, 0, 4), local(m_lay[(size_t)idx].label), pixelFont(font(), 11, false));
    }

    void paintSlot(QPainter &pnt, int idx)
    {
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(rgb(A52SESSION_SLOT_RGB));
        pnt.drawRoundedRect(rectOf(idx), 1.5, 1.5);
    }

    void paintWell(QPainter &pnt, int idx)
    {
        const int body = bodyIndex(m_lay[(size_t)idx].port);
        pnt.save();
        if (body >= 0) pnt.setClipPath(bodyPath(rectOf(body)));
        pnt.setPen(QPen(rgb(A52SESSION_CASE_SHADOW_RGB), 1));
        pnt.setBrush(rgb(A52SESSION_WELL_RGB));
        pnt.drawRoundedRect(rectOf(idx), 4, 4);
        pnt.restore();
    }

    /* A key's legend: the label, or while Map / Show bindings is on the
     * binding written on it (key over pad button). */
    QString legend(const a52_control &ctl, bool small)
    {
        if (!showBind() || ctl.target < 0) return local(ctl.label);
        const QString key = shorten(keyText(m_session, ctl.target));
        if (small) return key.isEmpty() ? QStringLiteral("—") : key;
        const QString pad = shorten(padText(m_session, ctl.target));
        if (key.isEmpty() && pad.isEmpty()) return QStringLiteral("—");
        return pad.isEmpty() ? key : (key.isEmpty() ? pad : key + QLatin1Char('\n') + pad);
    }

    /* A 12-key pad cap: a black key with a bevel and a yellow legend. */
    void paintKeycap(QPainter &pnt, int idx)
    {
        const a52_control &ctl = m_lay[(size_t)idx];
        const bool on = lit(idx);
        const bool over = !on && m_hover == idx && m_hold == None;
        const QRectF full = rectOf(idx);
        pnt.setOpacity(dimmed(ctl) ? kDimAlpha : 1.0);
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(rgb(A52SESSION_CASE_SHADOW_RGB));
        pnt.drawRoundedRect(full, 3, 3);
        const QRectF face = full.adjusted(0.5, 0.5, -0.5, -2);
        QLinearGradient grad(face.topLeft(), face.bottomLeft());
        grad.setColorAt(0, rgb(A52SESSION_KEY_RGB).lighter(over ? 190 : 150));
        grad.setColorAt(1, rgb(A52SESSION_KEY_RGB));
        pnt.setPen(QPen(rgb(A52SESSION_CASE_BEVEL_RGB), 1));
        pnt.setBrush(grad);
        pnt.drawRoundedRect(face, 3, 3);
        if (on) {
            QPainterPath shape;
            shape.addRoundedRect(face, 3, 3);
            held(pnt, shape);
        }
        pnt.setPen(rgb(A52SESSION_LEGEND_RGB));
        const bool bind = showBind();
        drawFitted(pnt, face.adjusted(1, 0, -1, 0), legend(ctl, false), pixelFont(font(), bind ? 9 : 13, !bind));
        pnt.setOpacity(1.0);
    }

    /* START / PAUSE / RESET: a black key with its coloured italic legend. */
    void paintConsoleKey(QPainter &pnt, int idx)
    {
        const a52_control &ctl = m_lay[(size_t)idx];
        const bool on = lit(idx);
        const bool over = !on && m_hover == idx && m_hold == None;
        const QRectF full = rectOf(idx);
        pnt.setOpacity(dimmed(ctl) ? kDimAlpha : 1.0);
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(rgb(A52SESSION_CASE_SHADOW_RGB));
        pnt.drawRoundedRect(full, 3, 3);
        const QRectF face = full.adjusted(0.5, 0.5, -0.5, -2);
        pnt.setPen(QPen(rgb(A52SESSION_CASE_BEVEL_RGB), 1));
        pnt.setBrush(rgb(A52SESSION_KEY_RGB).lighter(over ? 190 : 130));
        pnt.drawRoundedRect(face, 3, 3);
        if (on) {
            QPainterPath shape;
            shape.addRoundedRect(face, 3, 3);
            held(pnt, shape);
        }
        const int key = ctl.target - A52_TARGET_KEY(0);
        pnt.setPen(rgb(key == A52_KEY_START ? A52SESSION_START_RGB
                                            : key == A52_KEY_PAUSE ? A52SESSION_PAUSE_RGB : A52SESSION_RESET_RGB));
        const bool bind = showBind();
        drawFitted(pnt, face.adjusted(1, 0, -1, 0), legend(ctl, true), pixelFont(font(), bind ? 9 : 10, true, !bind));
        pnt.setOpacity(1.0);
    }

    /* A red side button; the two of a pair are one wire, so both light. */
    void paintFire(QPainter &pnt, int idx)
    {
        const a52_control &ctl = m_lay[(size_t)idx];
        const bool on = lit(idx);
        const bool over = !on && m_hover == idx && m_hold == None;
        const QRectF box = rectOf(idx);
        pnt.setOpacity(dimmed(ctl) ? kDimAlpha : 1.0);
        QLinearGradient grad(box.topLeft(), box.bottomLeft());
        grad.setColorAt(0, over ? rgb(A52SESSION_FIRE_HI_RGB).lighter(110) : rgb(A52SESSION_FIRE_HI_RGB));
        grad.setColorAt(1, rgb(A52SESSION_FIRE_RGB).darker(120));
        pnt.setPen(QPen(rgb(A52SESSION_FIRE_RGB).darker(220), 1));
        pnt.setBrush(grad);
        pnt.drawRoundedRect(box, 3, 3);
        if (on) {
            QPainterPath shape;
            shape.addRoundedRect(box, 3, 3);
            held(pnt, shape);
        }
        if (showBind()) {
            pnt.setPen(Qt::white);
            const QString text = legend(ctl, true);
            /* narrow part: the key stands on its side */
            pnt.save();
            pnt.translate(box.center());
            pnt.rotate(-90);
            drawFitted(pnt, QRectF(-box.height() / 2, -box.width() / 2, box.height(), box.width()), text,
                       pixelFont(font(), 9, true));
            pnt.restore();
        }
        pnt.setOpacity(1.0);
    }

    /* The stick's dish: a chamfered rim, the rubber boot and the ribbed cap,
     * which leans toward the deflection; in Map the picked direction is marked. */
    void paintDish(QPainter &pnt, int idx)
    {
        const a52_control &ctl = m_lay[(size_t)idx];
        const QRectF box = rectOf(idx);
        const QPointF mid = box.center();
        const qreal rad = box.width() / 2;
        int stickIdx = -1;
        for (int cur = idx; cur < m_count; ++cur)
            if (m_lay[(size_t)cur].kind == A52_CTL_STICK && m_lay[(size_t)cur].port == ctl.port) { stickIdx = cur; break; }
        const bool on = stickIdx >= 0 && !mapping() && m_held[(size_t)stickIdx] != 0;
        const bool over = stickIdx >= 0 && m_hover == stickIdx && m_hold == None;
        const bool dim = stickIdx >= 0 && dimmed(m_lay[(size_t)stickIdx]);
        pnt.setOpacity(dim ? kDimAlpha : 1.0);

        QRadialGradient rim(mid, rad);
        rim.setColorAt(0.80, rgb(A52SESSION_DISH_RIM_LO_RGB));
        rim.setColorAt(1.00, over ? rgb(A52SESSION_DISH_RIM_HI_RGB).lighter(120) : rgb(A52SESSION_DISH_RIM_HI_RGB));
        pnt.setPen(QPen(rgb(A52SESSION_CASE_SHADOW_RGB), 1));
        pnt.setBrush(rim);
        pnt.drawEllipse(mid, rad, rad);
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(rgb(A52SESSION_CASE_SHADOW_RGB));
        pnt.drawEllipse(mid, rad * 0.86, rad * 0.86);
        /* the boot, with its rings */
        pnt.setBrush(rgb(A52SESSION_STICK_BOOT_RGB));
        pnt.drawEllipse(mid, rad * 0.7, rad * 0.7);
        pnt.setBrush(Qt::NoBrush);
        pnt.setPen(QPen(rgb(A52SESSION_DISH_RIM_LO_RGB), 1));
        for (qreal ring = 0.5; ring < 0.7; ring += 0.07) pnt.drawEllipse(mid, rad * ring, rad * ring);

        /* the cap, tilted toward the stick */
        QPointF lean(0, 0);
        if (stickIdx >= 0) lean = QPointF(m_stick[ctl.port][0], m_stick[ctl.port][1]) / 127.0 * rad * 0.35;
        const QPointF cap = mid + lean;
        const qreal capRad = rad * 0.28;
        pnt.setPen(QPen(rgb(A52SESSION_DISH_RIM_LO_RGB), 1));
        pnt.setBrush(rgb(A52SESSION_STICK_SHAFT_RGB));
        if (!lean.isNull()) {            /* the shaft, from the boot to the cap */
            QPainterPath shaft;
            shaft.addEllipse(mid + lean * 0.5, capRad * 0.9, capRad * 0.9);
            pnt.drawPath(shaft);
        }
        QRadialGradient capGrad(cap - QPointF(capRad * 0.3, capRad * 0.3), capRad * 1.4);
        capGrad.setColorAt(0, rgb(A52SESSION_STICK_SHAFT_RGB).lighter(240));
        capGrad.setColorAt(1, rgb(A52SESSION_STICK_SHAFT_RGB));
        pnt.setBrush(capGrad);
        pnt.drawEllipse(cap, capRad, capRad);
        pnt.setBrush(Qt::NoBrush);
        pnt.setPen(QPen(rgb(A52SESSION_DISH_RIM_LO_RGB), 1));
        pnt.drawEllipse(cap, capRad * 0.66, capRad * 0.66);
        pnt.drawEllipse(cap, capRad * 0.33, capRad * 0.33);
        if (on) {
            QPainterPath shape;
            shape.addEllipse(mid, rad, rad);
            held(pnt, shape);
        }

        if (stickIdx >= 0 && showBind()) paintStickBindings(pnt, ctl.port, mid, rad);
        pnt.setOpacity(1.0);
    }

    /* The four directions' keys round the dish; the picked one is lit. */
    void paintStickBindings(QPainter &pnt, int port, const QPointF &mid, qreal rad)
    {
        static const int acts[4] = { A52_ACT_UP, A52_ACT_RIGHT, A52_ACT_DOWN, A52_ACT_LEFT };
        static const QPointF dirs[4] = { QPointF(0, -1), QPointF(1, 0), QPointF(0, 1), QPointF(-1, 0) };
        for (int dir = 0; dir < 4; ++dir) {
            const int target = A52_TARGET_PORT(port, acts[dir]);
            const QPointF at = mid + dirs[dir] * rad * 0.62;
            const QRectF cell(at.x() - rad * 0.34, at.y() - rad * 0.19, rad * 0.68, rad * 0.38);
            if (mapping() && target == m_map) {
                pnt.setPen(QPen(rgb(A52SESSION_HELD_RGB), 2));
                pnt.setBrush(QColor(0xFF, 0xB0, 0x4A, 90));
            } else {
                pnt.setPen(Qt::NoPen);
                pnt.setBrush(QColor(0, 0, 0, 160));
            }
            pnt.drawRoundedRect(cell, 3, 3);
            pnt.setPen(Qt::white);
            const QString key = shorten(keyText(m_session, target));
            drawFitted(pnt, cell, key.isEmpty() ? QStringLiteral("—") : key, pixelFont(font(), 9, true));
        }
    }

    a52session *m_session;
    std::function<void(int)> m_onPick;
    std::vector<a52_control> m_lay;
    int m_count = 0;
    int m_ports = 0;
    std::vector<char> m_held;        /* per layout entry: held as the console sees it */
    int m_stick[A52_PORTS][2] = {};  /* each port's stick, from the session */
    QButtonGroup *m_mouseGroup = nullptr;
    QRadioButton *m_radios[A52_PORTS] = {};
    QTimer m_nudge;
    bool m_showBindings = false;
    int m_map = -2;
    Hold m_hold = None;
    int m_holdIdx = -1;              /* the part the mouse holds */
    bool m_partDown = false;         /* the pointer is still over the part it pressed */
    int m_hover = -1;
};

/* ---- ControllersWindow --------------------------------------------------------------- */

class ControllersWindow : public QWidget {
public:
    ControllersWindow(a52session *session, QWidget *parent);
    void present();
    /* Settings changed: the port count and the pad rows may have */
    void refresh();

protected:
    bool event(QEvent *evt) override;
    void keyPressEvent(QKeyEvent *evt) override;
    void keyReleaseEvent(QKeyEvent *evt) override;
    void showEvent(QShowEvent *evt) override;
    void hideEvent(QHideEvent *evt) override;
    void changeEvent(QEvent *evt) override;
    /* Tab is a key of the console's, not focus traversal. */
    bool focusNextPrevChild(bool) override { return false; }

private:
    void setMapState(int state);
    void bound(const QString &what, const QString &stolen);
    void pollCapture();
    void pollSlow();
    void rebuildPads();
    void refreshPadPlayers();

    a52session *m_session;
    StageWidget *m_stage = nullptr;
    QWidget *m_padArea = nullptr;
    QVBoxLayout *m_padBox = nullptr;
    std::vector<QWidget *> m_padRows;
    std::vector<QLabel *> m_padEff;       /* each pad's "plays Controller N" */
    unsigned m_padGen = ~0u;
    int m_padPorts = 0;                   /* the port count the pad combos were built for */
    QPushButton *m_mapButton = nullptr;
    QPushButton *m_clearButton = nullptr;
    ElidedLabel *m_hint = nullptr;
    QTimer m_captureTimer;
    QTimer m_heldTimer;
    QTimer m_slowTimer;
    uint32_t m_swallow = 0;               /* the key Map took: its repeats and release too */
    /* -2 idle, -1 armed and waiting for a target, >= 0 waiting for a key or
     * gamepad button for that target. */
    int m_mapState = -2;
};

ControllersWindow::ControllersWindow(a52session *session, QWidget *parent)
    /* A dialog-type window, so a tiling compositor floats it: it is a panel
     * of fixed-size parts and nothing here wants more room. */
    : QWidget(parent, Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint | Qt::CustomizeWindowHint),
      m_session(session)
{
    setObjectName(QStringLiteral("controllers"));
    setWindowTitle(QStringLiteral("Controllers (F9)"));
    setProperty("a52HelpContext", QStringLiteral("controllers"));
    setFocusPolicy(Qt::StrongFocus);

    auto *root = new QVBoxLayout(this);
    /* Sized to its parts and kept there: a resizable panel gets tiled and its
     * parts stretched. */
    root->setSizeConstraint(QLayout::SetFixedSize);

    m_stage = new StageWidget(session, [this](int target) { setMapState(target); });
    m_stage->setObjectName(QStringLiteral("stage"));
    root->addWidget(m_stage, 0, Qt::AlignHCenter);

    /* Gamepads: one row per pad, rebuilt as they come and go, in room for a
     * fixed number so the window does not change size when one is plugged in */
    auto *pads = new QGroupBox(QStringLiteral("Gamepads"));
    auto *padsBox = new QVBoxLayout(pads);
    m_padArea = new QWidget;
    m_padBox = new QVBoxLayout(m_padArea);
    m_padBox->setContentsMargins(0, 0, 0, 0);
    m_padBox->setSpacing(2);
    const int rowHeight = QComboBox().sizeHint().height() + 2;
    m_padArea->setFixedHeight(rowHeight * kMaxPadRows + m_padBox->spacing() * (kMaxPadRows - 1));
    m_padBox->setAlignment(Qt::AlignTop);
    padsBox->addWidget(m_padArea);
    root->addWidget(pads);

    auto *mapRow = new QHBoxLayout;
    m_mapButton = new QPushButton(QStringLiteral("Map"));
    m_mapButton->setFocusPolicy(Qt::NoFocus);
    m_mapButton->setFixedWidth(qMax(m_mapButton->sizeHint().width(),
                                    m_mapButton->fontMetrics().horizontalAdvance(QStringLiteral("Done")) + 32));
    m_mapButton->setToolTip(QStringLiteral("Pick a part, then press a key or a gamepad button to give it"));
    connect(m_mapButton, &QPushButton::clicked, this, [this] { setMapState(m_mapState == -2 ? -1 : -2); });
    m_clearButton = new QPushButton(QStringLiteral("Clear"));
    m_clearButton->setFocusPolicy(Qt::NoFocus);
    m_clearButton->setToolTip(QStringLiteral("Take the picked control's key and gamepad button away"));
    connect(m_clearButton, &QPushButton::clicked, this, [this] {
        if (m_mapState < 0) return;
        a52session_binding_clear(m_session, m_mapState);
        m_hint->setText(QStringLiteral("Cleared. Press a key or a gamepad button to bind it again"));
        m_stage->update();
    });
    auto *defaults = new QPushButton(QStringLiteral("Defaults"));
    defaults->setFocusPolicy(Qt::NoFocus);
    defaults->setToolTip(QStringLiteral("Put every key and gamepad button back to its default"));
    connect(defaults, &QPushButton::clicked, this, [this] {
        a52session_bindings_reset(m_session);
        for (int pad = 0, n = a52session_gamepad_count(m_session); pad < n; pad++)
            a52session_gamepad_assign(m_session, pad, -1);
        setMapState(m_mapState == -2 ? -2 : -1);
        m_hint->setText(QStringLiteral("Every control is back to its default."));
    });
    auto *bindings = new QCheckBox(QStringLiteral("Show bindings"));
    bindings->setFocusPolicy(Qt::NoFocus);
    bindings->setToolTip(QStringLiteral("Show the key each part has (hover for its gamepad button)"));
    bindings->setChecked(a52session_get_int(session, "controllers_show_bindings", 0) > 0);
    connect(bindings, &QCheckBox::toggled, this, [this](bool checked) {
        a52session_set_int(m_session, "controllers_show_bindings", checked ? 1 : 0);
        m_stage->setShowBindings(checked);
    });
    m_stage->setShowBindings(bindings->isChecked());
    mapRow->addWidget(m_mapButton);
    mapRow->addWidget(m_clearButton);
    mapRow->addWidget(defaults);
    mapRow->addWidget(bindings);
    mapRow->addStretch(1);
    mapRow->addWidget(a52HelpButton(QStringLiteral("controllers")));
    root->addLayout(mapRow);

    m_hint = new ElidedLabel(true, 2);
    root->addWidget(m_hint);

    connect(&m_captureTimer, &QTimer::timeout, this, [this] { pollCapture(); });
    connect(&m_heldTimer, &QTimer::timeout, this, [this] { m_stage->pollHeld(); });
    connect(&m_slowTimer, &QTimer::timeout, this, [this] { pollSlow(); });
    setMapState(-2);
    m_stage->pollHeld();
}

/* ---- Map mode -------------------------------------------------------------------------- */

void ControllersWindow::setMapState(int state)
{
    /* Entering (or changing) Map: nothing may stay held by the mouse, and the
     * keyboard's holds go too -- Map swallows key-ups, so a key held into it
     * would stick. */
    m_stage->releaseAll();
    a52session_release_all(m_session);
    m_mapState = state;
    m_clearButton->setEnabled(state >= 0);
    if (state == -2) {
        m_captureTimer.stop();
        a52session_gamepad_capture_cancel(m_session);
        m_mapButton->setText(QStringLiteral("Map"));
        m_mapButton->setStyleSheet(QString());
        m_hint->setText(QStringLiteral("Drag a stick, press a key or a side button with the mouse to hold it."));
    } else if (state == -1) {
        m_captureTimer.stop();
        a52session_gamepad_capture_cancel(m_session);
        m_mapButton->setText(QStringLiteral("Done"));
        m_mapButton->setStyleSheet(QStringLiteral("background: %1; color: black;").arg(a52AccentColor().name()));
        m_hint->setText(QStringLiteral("Click a part to remap it (a side of the dish for a stick direction). Escape leaves Map."));
    } else {
        m_hint->setText(QStringLiteral("Press a key or a gamepad button for %1 (Escape cancels).")
                            .arg(local(a52_target_name(state))));
        a52session_gamepad_capture_begin(m_session);
        m_captureTimer.start(50);
    }
    m_stage->setMap(m_mapState);
    m_stage->pollHeld();
}

/* A binding was made: stay armed, since remapping several in a row is normal. */
void ControllersWindow::bound(const QString &what, const QString &stolen)
{
    setMapState(-1);
    m_hint->setText(stolen.isEmpty() ? QStringLiteral("Bound %1").arg(what)
                                     : QStringLiteral("Bound %1 (was %2)").arg(what, stolen));
}

void ControllersWindow::pollCapture()
{
    int button;
    if (m_mapState < 0) { m_captureTimer.stop(); return; }
    if (!a52session_gamepad_capture_poll(m_session, &button)) return;
    char stolen[128] = "";
    a52session_binding_set_button(m_session, m_mapState, button, stolen, sizeof stolen);
    bound(local(a52_pad_button_name(button)), local(stolen));
}

/* ---- polling ---------------------------------------------------------------------------- */

/* Hot-plugged pads, auto-assignment changes and a changed port count. */
void ControllersWindow::pollSlow()
{
    const unsigned gen = a52session_gamepad_generation(m_session);
    if (m_stage->reload() || gen != m_padGen || a52session_active_ports(m_session) != m_padPorts) {
        m_padGen = gen;
        rebuildPads();
    } else {
        refreshPadPlayers();
    }
}

/* ---- gamepads --------------------------------------------------------------------------- */

void ControllersWindow::refreshPadPlayers()
{
    for (int idx = 0; idx < (int)m_padEff.size(); ++idx) {
        const int eff = a52session_gamepad_effective_port(m_session, idx);
        const QString text = eff >= 0 ? QStringLiteral("plays Controller %1").arg(eff + 1)
                                      : QStringLiteral("plays no controller");
        if (m_padEff[(size_t)idx]->text() != text) m_padEff[(size_t)idx]->setText(text);
    }
}

/* One row per pad: its name, the controller it plays and the one it is set to. */
void ControllersWindow::rebuildPads()
{
    for (QWidget *row : m_padRows) delete row;
    m_padRows.clear();
    m_padEff.clear();
    m_padPorts = a52session_active_ports(m_session);
    const int count = a52session_gamepad_count(m_session);
    if (count == 0) {
        auto *none = new QLabel(QStringLiteral("No gamepads connected - plug one in"));
        none->setStyleSheet(QStringLiteral("color: gray;"));
        m_padBox->addWidget(none);
        m_padRows.push_back(none);
        return;
    }
    for (int idx = 0; idx < count && idx < kMaxPadRows; ++idx) {
        char name[128];
        a52session_gamepad_name(m_session, idx, name, sizeof name);
        auto *row = new QWidget;
        auto *line = new QHBoxLayout(row);
        line->setContentsMargins(0, 0, 0, 0);
        auto *label = new ElidedLabel(false);
        label->setText(local(name));
        label->setToolTip(local(name));
        auto *eff = new QLabel;
        eff->setStyleSheet(QStringLiteral("color: gray;"));
        /* -1 = automatic (connection order), 0.. = that controller */
        auto *box = new QComboBox;
        box->setFocusPolicy(Qt::NoFocus);
        box->addItem(QStringLiteral("Automatic"));
        for (int port = 0; port < m_padPorts; ++port) box->addItem(QStringLiteral("Controller %1").arg(port + 1));
        const int assign = a52session_gamepad_assignment(m_session, idx);
        box->setCurrentIndex(assign < 0 || assign >= m_padPorts ? 0 : assign + 1);
        connect(box, &QComboBox::currentIndexChanged, this, [this, idx](int sel) {
            a52session_gamepad_assign(m_session, idx, sel <= 0 ? -1 : sel - 1);
            refreshPadPlayers();
        });
        line->addWidget(label, 1);
        line->addWidget(eff);
        line->addWidget(box);
        m_padBox->addWidget(row);
        m_padRows.push_back(row);
        m_padEff.push_back(eff);
    }
    if (count > kMaxPadRows) {
        /* the rest are in Preferences > Input */
        m_padEff.back()->setToolTip(QStringLiteral("%1 more gamepads are listed in Preferences").arg(count - kMaxPadRows));
    }
    refreshPadPlayers();
}

/* ---- the window ------------------------------------------------------------------------- */

void ControllersWindow::present()
{
    m_stage->reload();
    m_padGen = a52session_gamepad_generation(m_session);
    rebuildPads();
    show();
    raise();
}

void ControllersWindow::refresh()
{
    if (!isVisible()) return;
    /* a new port count rebuilds the stage and lets go of the mouse's holds; a
     * plain refresh must not drop a drag in progress */
    if (m_stage->reload()) rebuildPads();
    m_stage->pollHeld();
    m_stage->update();
}

void ControllersWindow::showEvent(QShowEvent *evt)
{
    m_stage->pollHeld();
    m_heldTimer.start(40);      /* 25 Hz */
    m_slowTimer.start(250);
    QWidget::showEvent(evt);
}

void ControllersWindow::hideEvent(QHideEvent *evt)
{
    m_heldTimer.stop();
    m_slowTimer.stop();
    setMapState(-2);
    m_swallow = 0;
    QWidget::hideEvent(evt);
}

/* Losing the focus lets go of the mouse's holds and the keyboard's. */
void ControllersWindow::changeEvent(QEvent *evt)
{
    if (evt->type() == QEvent::ActivationChange && !isActiveWindow()) {
        a52session_release_pointer(m_session);
        m_stage->releaseAll();
        a52session_release_all(m_session);
        m_swallow = 0;
    }
    QWidget::changeEvent(evt);
}

/* F9 (this window), F11 and F12 are application-wide shortcuts. While Map
 * waits for a key they are taken here instead, so the hint can say they are
 * reserved (and so nothing but Escape closes Map). */
bool ControllersWindow::event(QEvent *evt)
{
    if (evt->type() == QEvent::ShortcutOverride && m_mapState >= 0) {
        evt->accept();
        return true;
    }
    return QWidget::event(evt);
}

void ControllersWindow::keyPressEvent(QKeyEvent *evt)
{
    /* F9, F11 and F12 are never bindings: in Map they only explain (outside it
     * the application shortcuts took them already) */
    if (evt->key() == Qt::Key_F9 || evt->key() == Qt::Key_F11 || evt->key() == Qt::Key_F12) {
        if (!evt->isAutoRepeat() && m_mapState >= 0)
            m_hint->setText(QStringLiteral("F9, F11 and F12 are reserved and cannot be bound."));
        return;
    }
    const uint32_t keysym = a52KeysymFromQt(evt);
    if (m_swallow && keysym == m_swallow) return;

    if (m_mapState != -2) {
        if (evt->isAutoRepeat()) return;
        /* Esc backs out one step: a pending target, then Map itself */
        if (evt->key() == Qt::Key_Escape) {
            m_swallow = keysym;
            setMapState(m_mapState >= 0 ? -1 : -2);
            return;
        }
        if (m_mapState < 0 || !keysym) return;      /* armed, or nothing to bind: nothing is typed */
        char name[48] = "", stolen[128] = "";
        a52session_binding_set_key(m_session, m_mapState, keysym, stolen, sizeof stolen);
        a52session_keysym_name(keysym, name, sizeof name);
        m_swallow = keysym;
        bound(local(name), local(stolen));
        return;
    }
    /* Otherwise what the main window does with it: a system action goes to
     * the main window's queue, as a gamepad's does. */
    const int sysact = a52KeySysaction(m_session, evt);
    if (sysact >= 0) { a52session_sysaction_post(m_session, sysact); return; }
    if (!a52ForwardKey(m_session, evt, true)) QWidget::keyPressEvent(evt);
}

/* A release always goes to the session, in Map mode too: it only lets go of
 * what a press asserted, so a key held while Map was armed cannot stick. */
void ControllersWindow::keyReleaseEvent(QKeyEvent *evt)
{
    const uint32_t keysym = a52KeysymFromQt(evt);
    if (m_swallow && keysym == m_swallow) {
        if (!evt->isAutoRepeat()) m_swallow = 0;
        return;
    }
    if (!a52ForwardKey(m_session, evt, false)) QWidget::keyReleaseEvent(evt);
}

QPointer<ControllersWindow> g_window;

} // namespace

void a52ControllersToggle(QWidget *parent, a52session *session)
{
    if (!g_window) g_window = new ControllersWindow(session, parent);
    if (g_window->isVisible()) g_window->hide();
    else g_window->present();
}

void a52ControllersShow(QWidget *parent, a52session *session)
{
    if (!g_window) g_window = new ControllersWindow(session, parent);
    if (!g_window->isVisible()) g_window->present();
    else g_window->raise();
}

bool a52ControllersVisible()
{
    return g_window && g_window->isVisible();
}

bool a52ControllersActive()
{
    return g_window && g_window->isActiveWindow();
}

void a52ControllersRefresh()
{
    if (g_window) g_window->refresh();
}
