/*
 * FujiNetWindows -- see FujiNetWindows.h.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "FujiNetWindows.h"
#include "help/HelpWindow.h"

#include <QColor>
#include <QDesktopServices>
#include <QFont>
#include <QPalette>
#include <QDialogButtonBox>
#include <QPlainTextEdit>
#include <QShortcut>
#include <QPointer>
#include <QScrollBar>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

QColor a52AccentColor()
{
    return QColor((A52SESSION_ACCENT_RGB >> 16) & 0xff,
                  (A52SESSION_ACCENT_RGB >> 8) & 0xff,
                  A52SESSION_ACCENT_RGB & 0xff);
}

QColor a52AccentTextColor(const QWidget *w)
{
    /* on a dark palette the orange is lightened to #E8A070 */
    const QColor bg = w ? w->palette().color(QPalette::Window) : QColor(Qt::white);
    return bg.lightness() < 128 ? QColor(0xe8, 0xa0, 0x70) : a52AccentColor();
}

void fujinet_log_show(QWidget *parent, a52session *session)
{
    static QPointer<QWidget> win;
    if (win) {
        win->raise();
        win->activateWindow();
        return;
    }
    win = new QWidget(parent, Qt::Window);
    win->setAttribute(Qt::WA_DeleteOnClose);
    win->setWindowTitle(QStringLiteral("FujiNet Console Log"));
    win->resize(820, 560);

    auto *view = new QPlainTextEdit(win);
    view->setReadOnly(true);
    QFont mono = view->font();
    mono.setFamily(QStringLiteral("monospace"));
    mono.setStyleHint(QFont::TypeWriter);
    view->setFont(mono);

    /* Help, and F1 for it */
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Help);
    buttons->setContentsMargins(8, 0, 8, 8);
    QWidget *w = win;
    QObject::connect(buttons, &QDialogButtonBox::helpRequested, w, [w] { a52HelpFor(w); });
    new QShortcut(QKeySequence(Qt::Key_F1), w, [w] { a52HelpFor(w); });
    win->setProperty("a52HelpContext", QStringLiteral("fujilog"));

    auto *layout = new QVBoxLayout(win);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(view);
    layout->addWidget(buttons);

    auto *timer = new QTimer(win);
    auto refresh = [view, session]() {
        static char buf[128 * 1024];
        const int n = a52session_fujinet_copy_log(session, buf, sizeof(buf));
        const QString text = n > 0 ? QString::fromUtf8(buf) : QStringLiteral("(no FujiNet output yet)");
        if (text == view->toPlainText())
            return;  // unchanged: leave the reader where they are
        QScrollBar *bar = view->verticalScrollBar();
        const int value = bar->value();
        const bool atEnd = value >= bar->maximum() - 2;
        view->setPlainText(text);
        bar->setValue(atEnd ? bar->maximum() : value);
    };
    QObject::connect(timer, &QTimer::timeout, view, refresh);
    timer->start(1000);
    refresh();
    win->show();
}

void fujinet_config_show(QWidget *parent, a52session *session)
{
    (void)parent;
    QDesktopServices::openUrl(QUrl(QString::fromUtf8(a52session_fujinet_webui_url(session))));
}
