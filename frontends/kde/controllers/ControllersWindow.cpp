/*
 * ControllersWindow -- see ControllersWindow.h.
 *
 * The 5200 controller as printed: the 12-key pad and START / PAUSE / RESET
 * (painted from a52session_keyboard_layout, so every frontend shows the same
 * caps), and a joystick per port (an analog stick you drag with the mouse,
 * four direction buttons, FIRE and FIRE2), the gamepads and their ports, and
 * Map mode.
 *
 * Keys and controls act on mouse press AND release, not "clicked": the
 * console samples its inputs once per frame, so a press present only for the
 * instant of a click falls between frames. A cap is HELD while the mouse is
 * down on it; dragging off it releases it; losing the focus, hiding and
 * rebuilding let go of what the mouse holds here (only that: the main window
 * releases its own keys). What the console sees held -- host keyboard,
 * gamepads and this panel -- is lit in the accent, so the panel doubles as an
 * input tester for a new pad.
 *
 * FIXED SIZE: fixed-size controls whose Map labels elide, and elided gamepad
 * names, so neither Map mode nor a long name resizes the window. A singleton,
 * hidden rather than destroyed, so a half-done remap survives closing it.
 *
 * MAP MODE: Map arms a two-step rebind -- click a stick direction, a fire
 * button or a cap, then press a key or a gamepad button. Rebinding steals the
 * key from whatever held it.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ControllersWindow.h"

#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QTimer>
#include <QToolTip>
#include <QtMath>
#include <QVBoxLayout>

#include <cstring>
#include <functional>
#include <utility>
#include <vector>

#include "../FujiNetWindows.h"
#include "../KeyForward.h"

namespace {

constexpr int kKeyUnit = 44;    /* pixels per keycap unit */
constexpr int kKeyGap = 4;      /* between key caps */
constexpr int kCellW = 70;      /* a stick direction / fire button */
constexpr int kCellH = 34;
constexpr int kCellGap = 4;
constexpr int kStick = 76;      /* the analog stick's pad */
constexpr int kMaxPads = 8;

const QString kGray = QStringLiteral("color: gray;");

QString accentStyle()
{
    return QStringLiteral("background: %1; color: white;").arg(a52AccentColor().name());
}

/* Map mode on a control: the key over the gamepad button. */
QString bindingText(a52session *s, int target)
{
    const a52_binding b = a52session_binding_get(s, target);
    char key[48];
    a52session_keysym_name(b.keysym, key, sizeof key);
    QString text = key[0] ? QString::fromUtf8(key) : QStringLiteral("—");
    if (b.button != A52_PAD_BTN_NONE)
        text += QLatin1Char('\n') + QString::fromUtf8(a52_pad_button_name(b.button));
    return text;
}

QString bindingTip(a52session *s, int target)
{
    const a52_binding b = a52session_binding_get(s, target);
    char key[48];
    a52session_keysym_name(b.keysym, key, sizeof key);
    QString tip = QStringLiteral("%1: %2").arg(QString::fromUtf8(a52_target_name(target)),
                                               key[0] ? QString::fromUtf8(key) : QStringLiteral("no key"));
    if (b.button != A52_PAD_BTN_NONE)
        tip += QStringLiteral(", ") + QString::fromUtf8(a52_pad_button_name(b.button));
    return tip;
}

QFont scaledFont(QFont f, qreal scale, bool bold = false)
{
    f.setPointSizeF((f.pointSizeF() > 0 ? f.pointSizeF() : 10) * scale);
    f.setBold(bold);
    return f;
}

/* Lines centred in r, the font shrunk until they fit. */
void drawFitted(QPainter &p, const QRectF &r, const QString &text, QFont f)
{
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (;;) {
        const QFontMetricsF fm(f);
        qreal w = 0;
        for (const QString &l : lines) w = qMax(w, fm.horizontalAdvance(l));
        if ((w <= r.width() && fm.lineSpacing() * lines.size() <= r.height() + 2)
            || f.pointSizeF() <= 5.5)
            break;
        f.setPointSizeF(f.pointSizeF() - 0.5);
    }
    p.setFont(f);
    p.drawText(r, Qt::AlignCenter, text);
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
    QSize sizeHint() const override { return QSize(0, fontMetrics().lineSpacing() * m_lines); }
    QSize minimumSizeHint() const override { return sizeHint(); }
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setPen(m_dim ? QColor(Qt::gray) : palette().color(QPalette::WindowText));
        if (m_lines > 1)
            p.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap, m_text);
        else
            p.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter,
                       fontMetrics().elidedText(m_text, Qt::ElideRight, width()));
    }
private:
    QString m_text;
    bool m_dim;
    int m_lines;
};

/* ---- PadButton -------------------------------------------------------------- */

/* A fixed-size button that reports press and release, not "clicked": a
 * stick direction is HELD, since the Atari samples it once per frame. */
class PadButton : public QPushButton {
public:
    PadButton(const QString &face, int target, std::function<void(PadButton *, bool)> onPress)
        : QPushButton(face), m_face(face), m_target(target), m_onPress(std::move(onPress))
    {
        setFocusPolicy(Qt::NoFocus);
        setFixedSize(kCellW, kCellH);
        m_baseFont = font();
    }
    int target() const { return m_target; }
    void setHeld(bool held)
    {
        if (held == m_held) return;
        m_held = held;
        setStyleSheet(held ? accentStyle() : QString());
    }
    /* Map mode: the binding, each line elided to the button. */
    void showBinding(a52session *s, bool isTarget)
    {
        setFont(scaledFont(m_baseFont, 0.85));
        QStringList lines = bindingText(s, m_target).split(QLatin1Char('\n'));
        for (QString &l : lines) l = fontMetrics().elidedText(l, Qt::ElideRight, width() - 8);
        setText(lines.join(QLatin1Char('\n')));
        setToolTip(bindingTip(s, m_target));
        setHeld(isTarget);
    }
    void showFace()
    {
        setFont(m_baseFont);
        setText(m_face);
        setToolTip(QString());
        setHeld(false);
    }
protected:
    /* Not QPushButton's own: its pressed look would hide the held fill. */
    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton && !m_down) {
            m_down = true;
            m_onPress(this, true);
        }
    }
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton && m_down) {
            m_down = false;
            m_onPress(this, false);
        }
    }
    /* Dragging off a button must release it. */
    void leaveEvent(QEvent *e) override
    {
        if (m_down) {
            m_down = false;
            m_onPress(this, false);
        }
        QPushButton::leaveEvent(e);
    }
private:
    QString m_face;
    int m_target;
    std::function<void(PadButton *, bool)> m_onPress;
    QFont m_baseFont;
    bool m_down = false;
    bool m_held = false;
};

/* ---- StickWidget -------------------------------------------------------------- */

/* The analog stick of one port: drag the knob (or anywhere in the pad); the
 * knob shows where the console sees the stick, from any source. */
class StickWidget : public QWidget {
public:
    StickWidget(a52session *session, int port) : m_session(session), m_port(port)
    {
        setFixedSize(kStick, kStick);
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::OpenHandCursor);
    }
    void setMapping(bool on)
    {
        m_mapping = on;
        setEnabled(!on);
    }
    /* repaints only when the stick moved */
    void poll()
    {
        int x, y;
        a52session_stick_get(m_session, m_port, &x, &y);
        if (x != m_x || y != m_y) {
            m_x = x;
            m_y = y;
            update();
        }
    }
    void letGo()
    {
        if (m_drag) {
            m_drag = false;
            a52session_stick_set(m_session, m_port, 0, 0);
        }
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QPalette pal = palette();
        const QRectF r = QRectF(rect()).adjusted(2, 2, -2, -2);
        p.setPen(pal.color(QPalette::Mid));
        p.setBrush(pal.color(QPalette::Base));
        p.drawEllipse(r);
        p.drawLine(QPointF(r.center().x(), r.top() + 4), QPointF(r.center().x(), r.bottom() - 4));
        p.drawLine(QPointF(r.left() + 4, r.center().y()), QPointF(r.right() - 4, r.center().y()));
        const qreal reach = r.width() / 2 - 9;
        const QPointF c = r.center() + QPointF(m_x / 127.0 * reach, m_y / 127.0 * reach);
        const bool off = m_x || m_y;
        p.setPen(pal.color(QPalette::Dark));
        p.setBrush(off ? a52AccentColor() : pal.color(QPalette::Button));
        p.drawEllipse(c, 9, 9);
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton) return;
        m_drag = true;
        setCursor(Qt::ClosedHandCursor);
        send(e->position());
    }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (m_drag) send(e->position());
    }
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton) return;
        setCursor(Qt::OpenHandCursor);
        letGo();
    }

private:
    void send(const QPointF &pos)
    {
        const qreal half = kStick / 2.0 - 11;
        const int x = qBound(-128, (int)((pos.x() - kStick / 2.0) / half * 127), 127);
        const int y = qBound(-128, (int)((pos.y() - kStick / 2.0) / half * 127), 127);
        a52session_stick_set(m_session, m_port, x, y);
    }

    a52session *m_session;
    int m_port;
    int m_x = 0, m_y = 0;
    bool m_drag = false;
    bool m_mapping = false;
};

/* ---- KeypadWidget ------------------------------------------------------------- */

/* The keypad and START / PAUSE / RESET, painted from
 * a52session_keyboard_layout (cap units). The window holds the key. */
class KeypadWidget : public QWidget {
public:
    KeypadWidget(a52session *session, std::function<void(int, bool)> onKey)
        : m_session(session), m_onKey(std::move(onKey))
    {
        setFocusPolicy(Qt::NoFocus);
        int n = 0;
        float w = 0, h = 0;
        const a52_keycap *caps = a52session_keyboard_layout(&n, &w, &h);
        m_caps.assign(caps, caps + qMax(0, n));
        m_held.assign(m_caps.size(), 0);
        setFixedSize(qCeil(w * kKeyUnit), qCeil(h * kKeyUnit));
    }
    const a52_keycap &cap(int i) const { return m_caps[(size_t)i]; }
    void setMapState(int state)
    {
        m_mapState = state;
        update();
    }
    /* What the console sees held (and the cap the mouse holds); repaints only
     * on a change. */
    void pollHeld(int mouseKey)
    {
        const unsigned keys = a52session_keys_held(m_session);
        bool changed = false;
        for (size_t i = 0; i < m_caps.size(); ++i) {
            const char on = m_caps[i].key == mouseKey || ((keys >> m_caps[i].key) & 1);
            if (on != m_held[i]) {
                m_held[i] = on;
                changed = true;
            }
        }
        if (changed) update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QPalette pal = palette();
        const QColor accent = a52AccentColor();
        const QFont keyFont = scaledFont(font(), 1.0, true);
        const QFont smallFont = scaledFont(font(), 0.7);
        const bool mapping = m_mapState != -2;
        for (int i = 0; i < (int)m_caps.size(); ++i) {
            const a52_keycap &k = m_caps[(size_t)i];
            const QRectF r = capRect(i);
            const int t = A52_TARGET_KEY(k.key);
            const bool fill = mapping ? t == m_mapState : m_held[(size_t)i] != 0;
            const bool fn = k.kind == A52_KEYCAP_FUNCTION;
            p.setPen(pal.color(QPalette::Mid));
            p.setBrush(fill ? accent : pal.color(fn ? QPalette::Window : QPalette::Button));
            p.drawRoundedRect(r, 5, 5);
            p.setPen(fill ? QColor(Qt::white) : pal.color(QPalette::ButtonText));
            const QRectF in = r.adjusted(2, 1, -2, -1);
            if (mapping)
                drawFitted(p, in, bindingText(m_session, t), smallFont);
            else
                drawFitted(p, in, QString::fromUtf8(k.label), fn ? smallFont : keyFont);
        }
    }

    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton) return;
        const int i = capAt(e->position());
        if (i < 0) return;
        m_pressed = i;
        m_onKey(i, true);
    }
    /* Dragging off a cap must release it. */
    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (m_pressed >= 0 && !capRect(m_pressed).contains(e->position())) release();
    }
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton) release();
    }
    bool event(QEvent *e) override
    {
        if (e->type() == QEvent::ToolTip) {
            auto *he = static_cast<QHelpEvent *>(e);
            const int i = capAt(he->pos());
            if (i >= 0 && m_mapState != -2)
                QToolTip::showText(he->globalPos(), bindingTip(m_session, A52_TARGET_KEY(m_caps[(size_t)i].key)),
                                   this, capRect(i).toRect());
            else
                QToolTip::hideText();
            return true;
        }
        return QWidget::event(e);
    }

private:
    QRectF capRect(int i) const
    {
        const a52_keycap &k = m_caps[(size_t)i];
        return QRectF(k.x * kKeyUnit + kKeyGap / 2.0, k.y * kKeyUnit + kKeyGap / 2.0,
                      k.w * kKeyUnit - kKeyGap, k.h * kKeyUnit - kKeyGap);
    }
    int capAt(const QPointF &pos) const
    {
        for (int i = 0; i < (int)m_caps.size(); ++i)
            if (capRect(i).contains(pos)) return i;
        return -1;
    }
    void release()
    {
        if (m_pressed < 0) return;
        const int i = m_pressed;
        m_pressed = -1;
        m_onKey(i, false);
    }

    a52session *m_session;
    std::function<void(int, bool)> m_onKey;
    std::vector<a52_keycap> m_caps;
    std::vector<char> m_held;
    int m_mapState = -2;
    int m_pressed = -1;             /* cap index the mouse is on, or -1 */
};

/* ---- ControllersWindow -------------------------------------------------------- */

class ControllersWindow : public QWidget {
public:
    ControllersWindow(a52session *session, QWidget *parent);
    void present();

protected:
    void keyPressEvent(QKeyEvent *e) override;
    void keyReleaseEvent(QKeyEvent *e) override;
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;
    void changeEvent(QEvent *e) override;
    /* Tab is not focus traversal here. */
    bool focusNextPrevChild(bool) override { return false; }

private:
    QWidget *buildPort(int port);
    PadButton *control(const QString &face, int target);
    void onControl(int target, bool down);
    void onKey(int pos, bool down);
    void letGo();
    void setMapState(int state);
    void refreshLabels();
    void pollCapture();
    void pollHeld();
    void pollSlow();
    void rebuildPads();
    void refreshPadPorts();

    a52session *m_session;
    QVBoxLayout *m_root = nullptr;
    KeypadWidget *m_keypad = nullptr;
    StickWidget *m_stick[A52_PORTS] = {};
    std::vector<PadButton *> m_controls;            /* stick directions and fire buttons */
    QVBoxLayout *m_padBox = nullptr;
    std::vector<QWidget *> m_padRows;
    std::vector<QLabel *> m_padEff;                 /* each pad's "drives port N" */
    unsigned m_padGen = ~0u;
    QPushButton *m_mapButton = nullptr;
    ElidedLabel *m_hint = nullptr;
    QTimer m_captureTimer;
    QTimer m_heldTimer;
    QTimer m_slowTimer;
    int m_mouseKey = -1;        /* a52_key the mouse holds on the pad */
    int m_mouseTarget = -1;     /* control target the mouse holds */
    uint32_t m_swallow = 0;     /* the key Map took: its repeats and release too */
    /* -2 idle, -1 armed and waiting for a target, >=0 waiting for a key or
     * gamepad button. */
    int m_mapState = -2;
};

ControllersWindow::ControllersWindow(a52session *session, QWidget *parent)
    /* A dialog-type window, so a tiling compositor floats it: it is a
     * panel of fixed-size keys and buttons. */
    : QWidget(parent, Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint
                      | Qt::CustomizeWindowHint), m_session(session)
{
    setObjectName(QStringLiteral("controllers"));
    setWindowTitle(QStringLiteral("Controllers"));
    setFocusPolicy(Qt::StrongFocus);

    m_root = new QVBoxLayout(this);
    m_root->setSizeConstraint(QLayout::SetFixedSize);

    /* four ports, two to a row; the keypad is common to all of them */
    auto *ports = new QWidget;
    auto *grid = new QGridLayout(ports);
    grid->setContentsMargins(0, 0, 0, 0);
    for (int port = 0; port < A52_PORTS; ++port)
        grid->addWidget(buildPort(port), port / 2, port % 2);
    m_root->addWidget(ports, 0, Qt::AlignHCenter);

    m_keypad = new KeypadWidget(m_session, [this](int pos, bool down) { onKey(pos, down); });
    auto *padBox = new QGroupBox(QStringLiteral("Keypad (all controllers)"));
    auto *pv = new QVBoxLayout(padBox);
    pv->addWidget(m_keypad, 0, Qt::AlignHCenter);
    m_root->addWidget(padBox);

    /* Gamepads: one row per pad, rebuilt as they come and go */
    auto *pads = new QGroupBox(QStringLiteral("Gamepads"));
    m_padBox = new QVBoxLayout(pads);
    m_root->addWidget(pads);

    auto *maprow = new QHBoxLayout;
    m_mapButton = new QPushButton(QStringLiteral("Map"));
    m_mapButton->setFocusPolicy(Qt::NoFocus);
    m_mapButton->setFixedWidth(qMax(m_mapButton->sizeHint().width(),
                                    m_mapButton->fontMetrics().horizontalAdvance(QStringLiteral("Done")) + 32));
    connect(m_mapButton, &QPushButton::clicked, this, [this] { setMapState(m_mapState == -2 ? -1 : -2); });
    auto *defaults = new QPushButton(QStringLiteral("Defaults"));
    defaults->setFocusPolicy(Qt::NoFocus);
    connect(defaults, &QPushButton::clicked, this, [this] {
        a52session_bindings_reset(m_session);
        refreshLabels();
    });
    m_hint = new ElidedLabel(true, 2);
    maprow->addWidget(m_mapButton);
    maprow->addWidget(defaults);
    maprow->addWidget(m_hint, 1);
    m_root->addLayout(maprow);

    connect(&m_captureTimer, &QTimer::timeout, this, [this] { pollCapture(); });
    connect(&m_heldTimer, &QTimer::timeout, this, [this] { pollHeld(); });
    connect(&m_slowTimer, &QTimer::timeout, this, [this] { pollSlow(); });
    setMapState(-2);
}

/* One port: the analog stick between its four direction buttons, and the two
 * fire buttons in the top corners. The radio says which port the mouse drags
 * in the display move. */
QWidget *ControllersWindow::buildPort(int port)
{
    auto *box = new QGroupBox(QStringLiteral("Controller %1").arg(port + 1));
    auto *v = new QVBoxLayout(box);
    auto *w = new QWidget;
    auto *grid = new QGridLayout(w);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(kCellGap);
    m_stick[port] = new StickWidget(m_session, port);
    grid->addWidget(control(QStringLiteral("Fire 2"), A52_TARGET_PORT(port, A52_ACT_FIRE2)), 0, 0);
    grid->addWidget(control(QStringLiteral("▲"), A52_TARGET_PORT(port, A52_ACT_UP)), 0, 1);
    grid->addWidget(control(QStringLiteral("Fire"), A52_TARGET_PORT(port, A52_ACT_FIRE)), 0, 2);
    grid->addWidget(control(QStringLiteral("◀"), A52_TARGET_PORT(port, A52_ACT_LEFT)), 1, 0);
    grid->addWidget(m_stick[port], 1, 1, Qt::AlignCenter);
    grid->addWidget(control(QStringLiteral("▶"), A52_TARGET_PORT(port, A52_ACT_RIGHT)), 1, 2);
    grid->addWidget(control(QStringLiteral("▼"), A52_TARGET_PORT(port, A52_ACT_DOWN)), 2, 1);
    v->addWidget(w, 0, Qt::AlignHCenter);
    auto *mouse = new QRadioButton(QStringLiteral("Mouse drag in the display"));
    mouse->setFocusPolicy(Qt::NoFocus);
    mouse->setAutoExclusive(false);
    mouse->setChecked(a52session_get_int(m_session, "mouse_port", 0) == port);
    mouse->setProperty("a52-port", port);
    connect(mouse, &QRadioButton::clicked, this, [this, port, box] {
        a52session_set_int(m_session, "mouse_port", port);
        for (QRadioButton *r : window()->findChildren<QRadioButton *>())
            r->setChecked(r->property("a52-port").toInt() == port);
        (void)box;
    });
    v->addWidget(mouse, 0, Qt::AlignHCenter);
    return box;
}

PadButton *ControllersWindow::control(const QString &face, int target)
{
    auto *b = new PadButton(face, target, [this](PadButton *pb, bool down) { onControl(pb->target(), down); });
    m_controls.push_back(b);
    return b;
}

/* ---- pressing ------------------------------------------------------------------ */

void ControllersWindow::onControl(int target, bool down)
{
    if (m_mapState != -2) {
        if (down) setMapState(target);    /* armed: picks (or changes) the target */
        return;
    }
    if (down) {
        letGo();
        m_mouseTarget = target;
        a52session_press(m_session, target, 1);
    } else if (m_mouseTarget == target) {
        m_mouseTarget = -1;
        a52session_press(m_session, target, 0);
    }
    pollHeld();
}

void ControllersWindow::onKey(int pos, bool down)
{
    const int key = m_keypad->cap(pos).key;
    if (m_mapState != -2) {
        if (down) setMapState(A52_TARGET_KEY(key));
        return;
    }
    if (down) {
        letGo();
        m_mouseKey = key;
        a52session_press(m_session, A52_TARGET_KEY(key), 1);
    } else if (m_mouseKey == key) {
        m_mouseKey = -1;
        a52session_press(m_session, A52_TARGET_KEY(key), 0);
    }
    pollHeld();
}

/* Lets go of everything the mouse holds in this panel. */
void ControllersWindow::letGo()
{
    if (m_mouseKey >= 0) {
        a52session_press(m_session, A52_TARGET_KEY(m_mouseKey), 0);
        m_mouseKey = -1;
    }
    if (m_mouseTarget >= 0) {
        a52session_press(m_session, m_mouseTarget, 0);
        m_mouseTarget = -1;
    }
    for (StickWidget *s : m_stick)
        if (s) s->letGo();
}

/* ---- Map mode ------------------------------------------------------------------ */

void ControllersWindow::setMapState(int state)
{
    m_mapState = state;
    if (state == -2) {
        m_captureTimer.stop();
        a52session_gamepad_capture_cancel(m_session);
        m_mapButton->setText(QStringLiteral("Map"));
        m_mapButton->setStyleSheet(QString());
        m_hint->setText(QStringLiteral("Click and hold to press. Drag a stick for the analog range."));
    } else if (state == -1) {
        m_captureTimer.stop();
        a52session_gamepad_capture_cancel(m_session);
        m_mapButton->setText(QStringLiteral("Done"));
        m_mapButton->setStyleSheet(accentStyle());
        m_hint->setText(QStringLiteral("Click a stick direction, a fire button or a key to remap"));
    } else {
        letGo();
        m_hint->setText(QStringLiteral("Press a key or gamepad button for %1")
                            .arg(QString::fromUtf8(a52_target_name(state))));
        a52session_gamepad_capture_begin(m_session);
        m_captureTimer.start(50);
    }
    refreshLabels();
}

void ControllersWindow::pollCapture()
{
    int button;
    if (m_mapState < 0) { m_captureTimer.stop(); return; }
    if (!a52session_gamepad_capture_poll(m_session, &button)) return;
    char stolen[128] = "";
    a52session_binding_set_button(m_session, m_mapState, button, stolen, sizeof stolen);
    const QString name = QString::fromUtf8(a52_pad_button_name(button));
    setMapState(-1);    /* stay armed: remapping several in a row is normal */
    m_hint->setText(stolen[0] ? QStringLiteral("Bound %1 (was %2)").arg(name, QString::fromUtf8(stolen))
                              : QStringLiteral("Bound %1").arg(name));
}

/* In Map mode each remappable control shows what is bound to it, so
 * choosing what to change does not take remembering the whole table. */
void ControllersWindow::refreshLabels()
{
    const bool mapping = m_mapState != -2;
    for (PadButton *b : m_controls) {
        if (mapping) b->showBinding(m_session, b->target() == m_mapState);
        else b->showFace();
    }
    for (StickWidget *s : m_stick)
        if (s) s->setMapping(mapping);
    m_keypad->setMapState(m_mapState);
    pollHeld();
}

/* ---- polling ------------------------------------------------------------------- */

/* What the console sees held, from every source, lit (~30 Hz). */
void ControllersWindow::pollHeld()
{
    for (StickWidget *s : m_stick)
        if (s) s->poll();
    if (m_mapState != -2) return;
    unsigned held[A52_PORTS];
    for (int port = 0; port < A52_PORTS; ++port)
        held[port] = a52session_buttons_held(m_session, port);
    for (PadButton *b : m_controls) {
        const int t = b->target();
        b->setHeld(t == m_mouseTarget
                   || ((held[t / A52_ACT_PER_PORT] >> (t % A52_ACT_PER_PORT)) & 1));
    }
    m_keypad->pollHeld(m_mouseKey);
}

/* Hot-plugged pads and auto-assignment changes. */
void ControllersWindow::pollSlow()
{
    const unsigned gen = a52session_gamepad_generation(m_session);
    if (gen != m_padGen) {
        m_padGen = gen;
        rebuildPads();
    } else {
        refreshPadPorts();
    }
}

/* ---- gamepads ------------------------------------------------------------------ */

void ControllersWindow::refreshPadPorts()
{
    for (int i = 0; i < (int)m_padEff.size(); ++i) {
        const int eff = a52session_gamepad_effective_port(m_session, i);
        const QString text = eff >= 0 ? QStringLiteral("drives controller %1").arg(eff + 1)
                                      : QStringLiteral("drives nothing");
        if (m_padEff[(size_t)i]->text() != text) m_padEff[(size_t)i]->setText(text);
    }
}

/* One row per pad: its name, the port it drives and the port it is set to. */
void ControllersWindow::rebuildPads()
{
    for (QWidget *w : m_padRows) delete w;
    m_padRows.clear();
    m_padEff.clear();
    const int n = a52session_gamepad_count(m_session);
    if (n == 0) {
        auto *none = new QLabel(QStringLiteral("No gamepads connected"));
        none->setStyleSheet(kGray);
        m_padBox->addWidget(none);
        m_padRows.push_back(none);
        return;
    }
    for (int i = 0; i < n && i < kMaxPads; ++i) {
        char name[128];
        a52session_gamepad_name(m_session, i, name, sizeof name);
        auto *row = new QWidget;
        auto *h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        auto *label = new ElidedLabel(false);
        label->setText(QString::fromUtf8(name));
        label->setToolTip(QString::fromUtf8(name));
        auto *eff = new QLabel;
        eff->setStyleSheet(kGray);
        /* 0 = automatic (connection order), 1-4 = that port */
        auto *box = new QComboBox;
        box->setFocusPolicy(Qt::NoFocus);
        box->addItems({ QStringLiteral("Automatic"), QStringLiteral("Controller 1"), QStringLiteral("Controller 2"),
                        QStringLiteral("Controller 3"), QStringLiteral("Controller 4") });
        const int assign = a52session_gamepad_assignment(m_session, i);
        box->setCurrentIndex(assign < 0 || assign >= A52_PORTS ? 0 : assign + 1);
        connect(box, &QComboBox::currentIndexChanged, this, [this, i](int idx) {
            a52session_gamepad_assign(m_session, i, idx <= 0 ? -1 : idx - 1);
            refreshPadPorts();
        });
        h->addWidget(label, 1);
        h->addWidget(eff);
        h->addWidget(box);
        m_padBox->addWidget(row);
        m_padRows.push_back(row);
        m_padEff.push_back(eff);
    }
    refreshPadPorts();
}

/* ---- the window ----------------------------------------------------------------- */

void ControllersWindow::present()
{
    m_padGen = a52session_gamepad_generation(m_session);
    rebuildPads();
    show();
    raise();
}

void ControllersWindow::showEvent(QShowEvent *e)
{
    pollHeld();
    m_heldTimer.start(33);
    m_slowTimer.start(250);
    QWidget::showEvent(e);
}

void ControllersWindow::hideEvent(QHideEvent *e)
{
    m_heldTimer.stop();
    m_slowTimer.stop();
    setMapState(-2);
    letGo();
    QWidget::hideEvent(e);
}

/* Only what this panel's mouse holds: the main window releases its own. */
void ControllersWindow::changeEvent(QEvent *e)
{
    if (e->type() == QEvent::ActivationChange && !isActiveWindow()) letGo();
    QWidget::changeEvent(e);
}

void ControllersWindow::keyPressEvent(QKeyEvent *e)
{
    const uint32_t ks = a52KeysymFromQt(e);
    if (m_swallow && ks == m_swallow) return;
    if (m_mapState >= 0) {
        if (e->isAutoRepeat() || !ks) return;
        char stolen[128] = "", name[48] = "";
        a52session_binding_set_key(m_session, m_mapState, ks, stolen, sizeof stolen);
        a52session_keysym_name(ks, name, sizeof name);
        m_swallow = ks;
        setMapState(-1);    /* stay armed: remapping several in a row is normal */
        m_hint->setText(stolen[0] ? QStringLiteral("Bound %1 (was %2)").arg(QString::fromUtf8(name), QString::fromUtf8(stolen))
                                  : QStringLiteral("Bound %1").arg(QString::fromUtf8(name)));
        return;
    }
    /* Otherwise what the main window does with it; a system action goes
     * to the main window's queue, as a gamepad's does. */
    const int sa = a52KeySysaction(m_session, e);
    if (sa >= 0) {
        a52session_sysaction_post(m_session, sa);
        return;
    }
    if (!a52ForwardKey(m_session, e, true)) QWidget::keyPressEvent(e);
}

void ControllersWindow::keyReleaseEvent(QKeyEvent *e)
{
    const uint32_t ks = a52KeysymFromQt(e);
    if (m_swallow && ks == m_swallow) {
        if (!e->isAutoRepeat()) m_swallow = 0;
        return;
    }
    if (!a52ForwardKey(m_session, e, false)) QWidget::keyReleaseEvent(e);
}

QPointer<ControllersWindow> g_window;

} // namespace

void a52ControllersToggle(QWidget *parent, a52session *session)
{
    if (!g_window) g_window = new ControllersWindow(session, parent);
    if (g_window->isVisible()) g_window->hide();
    else g_window->present();
}

bool a52ControllersVisible()
{
    return g_window && g_window->isVisible();
}
