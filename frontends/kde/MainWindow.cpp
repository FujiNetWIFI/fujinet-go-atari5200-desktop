/*
 * MainWindow -- see MainWindow.h.
 *
 * Plain Qt6 Widgets, deliberately not KDE Frameworks: it picks up Breeze
 * through the platform theme anyway, and staying framework-free keeps this
 * frontend usable outside a KDE session.
 *
 * Keys go to the Atari pressed and released (KeyForward). The Atari's
 * CONTROL+letter keys are its graphics characters, so the app's shortcuts
 * are CTRL+SHIFT ones, plus F9, F11, F12 and Shift+F5 -- application-wide,
 * so they work from the Controllers panel and the debugger too.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "MainWindow.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QStatusBar>
#include <QUrl>
#include <QVBoxLayout>

#include "DisplayWidget.h"
#include "SoftDisplayWidget.h"
#include "FujiNetWindows.h"
#include "KeyForward.h"
#include "SettingsDialog.h"
#include "controllers/ControllersWindow.h"
#include "debugger/DebuggerWindow.h"
#include "help/HelpWindow.h"

static const char *const kAnyFilter =
    "5200 cartridges and BIOS images (*.a52 *.bin *.rom *.car *.zip);;All files (*)";

static QString local(const char *s) { return QString::fromUtf8(s ? s : ""); }

MainWindow::MainWindow(a52session *session, QWidget *parent)
    : QMainWindow(parent), m_session(session)
{
    setWindowTitle(QStringLiteral("FujiNet Go Atari 5200"));
    setProperty("a52HelpContext", QStringLiteral("main"));
    /* 336 x 240 at three times, a TV's shape */
    resize(864, 720 + 60);
    setAcceptDrops(true);

    auto *gpu = new DisplayWidget(session, this);
    m_display = gpu;
    setCentralWidget(gpu);
    /* No usable GPU device: fall back to painting in software */
    connect(gpu, &QRhiWidget::renderFailed, this, &MainWindow::useSoftwareDisplay,
            Qt::QueuedConnection);
    gpu->setFocus();

    m_dot = new QLabel(QStringLiteral("●"));
    m_dot->setToolTip(QStringLiteral("FujiNet link"));
    m_status = new QLabel(QStringLiteral("Starting..."));
    statusBar()->addWidget(m_dot);
    statusBar()->addWidget(m_status, 1);

    buildMenus();
    applyView();

    connect(&m_statusTimer, &QTimer::timeout, this, &MainWindow::updateStatus);
    m_statusTimer.start(1000);
    connect(&m_fastTimer, &QTimer::timeout, this, &MainWindow::fastTick);
    m_fastTimer.start(100);
    /* the current pads and FujiNet state are the baseline: no messages */
    m_padGeneration = a52session_gamepad_generation(session);
    m_fujinetGeneration = a52session_fujinet_generation(session);
    updateStatus();

    /* A52_OPEN_CONTROLLERS / _DEBUGGER / _SETTINGS open those windows at
     * launch; A52_SCREENSHOT=file.png saves the picture after
     * A52_SCREENSHOT_FRAMES frames (300); A52_AUTOQUIT_FRAMES quits. */
    if (qEnvironmentVariableIsSet("A52_OPEN_CONTROLLERS"))
        QTimer::singleShot(0, this, [this] { a52ControllersToggle(this, m_session); });
    if (qEnvironmentVariableIsSet("A52_OPEN_DEBUGGER"))
        QTimer::singleShot(0, this, [this] { a52DebuggerShow(this, m_session); });
    if (qEnvironmentVariableIsSet("A52_OPEN_SETTINGS"))
        QTimer::singleShot(0, this, &MainWindow::showSettings);
    m_shotPath = qEnvironmentVariable("A52_SCREENSHOT");
    m_shotFrames = qEnvironmentVariableIsSet("A52_SCREENSHOT_FRAMES")
                       ? qEnvironmentVariable("A52_SCREENSHOT_FRAMES").toULongLong() : 300;
    m_quitFrames = qEnvironmentVariable("A52_AUTOQUIT_FRAMES").toULongLong();
}

void MainWindow::toast(const QString &text, int ms)
{
    statusBar()->showMessage(text, ms);
}

/* ---- menus ---------------------------------------------------------------------- */

static QAction *appShortcut(QWidget *owner, QAction *a, const QKeySequence &k)
{
    /* application-wide: the panels have the keyboard too */
    a->setShortcut(k);
    a->setShortcutContext(Qt::ApplicationShortcut);
    owner->addAction(a);
    return a;
}

void MainWindow::buildMenus()
{
    m_machine = menuBar()->addMenu(QStringLiteral("&Console"));
    m_machine->addAction(QStringLiteral("&Open..."), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O), this, [this] {
        const QString f = QFileDialog::getOpenFileName(this, QStringLiteral("Open"), mediaFolder(),
                                                       QString::fromUtf8(kAnyFilter));
        if (!f.isEmpty())
            loadMedia(f);
    });
    m_machine->addSeparator();
    /* the 5200 has no reset line to its cartridge: power is how CONFIG comes back */
    appShortcut(this, m_machine->addAction(QStringLiteral("&Power Cycle (Reboot to CONFIG)"), this,
                                           &MainWindow::powerCycle),
                QKeySequence(Qt::SHIFT | Qt::Key_F5));
    /* F5 itself is the controller's RESET key through the bindings */
    m_machine->addAction(QStringLiteral("&RESET Key (F5)"), this, [this] { a52session_reset(m_session); });
    m_pauseAction = m_machine->addAction(QStringLiteral("&Pause Emulation"));
    m_pauseAction->setCheckable(true);
    connect(m_pauseAction, &QAction::triggered, this,
            [this](bool on) { a52session_set_paused(m_session, on); });
    m_warpAction = m_machine->addAction(QStringLiteral("&Warp Speed"));
    m_warpAction->setCheckable(true);
    m_warpAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_W));
    connect(m_warpAction, &QAction::triggered, this, [this](bool on) {
        a52session_set_warp(m_session, on);
        updateStatus();
    });
    m_machine->addSeparator();
    QMenu *bios = m_machine->addMenu(QStringLiteral("Import &BIOS"));
    bios->addAction(QStringLiteral("From &Files..."), this, [this] { importBios(0); });
    bios->addAction(QStringLiteral("From a &ZIP..."), this, [this] { importBios(1); });
    bios->addAction(QStringLiteral("From a F&older..."), this, [this] { importBios(2); });
    m_machine->addSeparator();
    m_machine->addAction(QStringLiteral("P&references..."), QKeySequence(Qt::CTRL | Qt::Key_Comma), this,
                         &MainWindow::showSettings);
    m_machine->addSeparator();
    m_machine->addAction(QStringLiteral("&Quit"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Q), this,
                         [this] { close(); });
    connect(m_machine, &QMenu::aboutToShow, this, [this] {
        m_warpAction->setChecked(a52session_warp(m_session));
        m_pauseAction->setChecked(a52session_is_paused(m_session));
    });

    QMenu *view = menuBar()->addMenu(QStringLiteral("&View"));
    appShortcut(this, view->addAction(QStringLiteral("C&ontrollers"), this,
                                      [this] { a52ControllersToggle(this, m_session); }),
                QKeySequence(Qt::Key_F9));
    appShortcut(this, view->addAction(QStringLiteral("&Debugger"), this,
                                      [this] { a52DebuggerToggle(this, m_session); }),
                QKeySequence(Qt::Key_F12));
    view->addSeparator();
    m_tvAction = view->addAction(QStringLiteral("&TV Aspect"));
    m_tvAction->setCheckable(true);
    m_tvAction->setToolTip(QStringLiteral("A TV's shape; off shows square pixels"));
    connect(m_tvAction, &QAction::triggered, this, [this](bool on) {
        const int i = a52session_pref_find("aspect");
        if (i >= 0)
            a52session_pref_set_choice(m_session, i, on ? 0 : 1);
        applyView();
    });
    m_smoothAction = view->addAction(QStringLiteral("&Smooth Scaling"));
    m_smoothAction->setCheckable(true);
    connect(m_smoothAction, &QAction::triggered, this, [this](bool on) {
        a52session_set_int(m_session, "smooth", on);
        applyView();
    });
    appShortcut(this, view->addAction(QStringLiteral("&Fullscreen"), this, [this] {
        if (isFullScreen()) showNormal(); else showFullScreen();
    }), QKeySequence(Qt::Key_F11));

    QMenu *fuji = menuBar()->addMenu(QStringLiteral("&FujiNet"));
    fuji->addAction(QStringLiteral("&Web UI"), this, [this] {
        if (a52session_fujinet_external(m_session)) {
            toast(QStringLiteral("FujiNet is external: use its own web UI"));
            return;
        }
        if (!a52session_fujinet_running(m_session)) {
            toast(QStringLiteral("FujiNet is off (Preferences ▸ FujiNet)"));
            return;
        }
        fujinet_config_show(this, m_session);
    });
    fuji->addAction(QStringLiteral("Console &Log"), this, [this] {
        if (a52session_fujinet_external(m_session)) {
            toast(QStringLiteral("FujiNet is external: see its own console"));
            return;
        }
        fujinet_log_show(this, m_session);
    });
    fuji->addAction(QStringLiteral("&Restart FujiNet"), this, [this] {
        if (a52session_fujinet_external(m_session)) {
            toast(QStringLiteral("FujiNet is external: restart it where it runs"));
            return;
        }
        if (a52session_fujinet_restart(m_session) != 0)
            QMessageBox::warning(this, QStringLiteral("Cannot Restart FujiNet"),
                                 local(a52session_last_error(m_session)));
        else
            toast(QStringLiteral("FujiNet restarted"), 3000);
    });
    fuji->addSeparator();
    fuji->addAction(QStringLiteral("&Import to SD Card..."), this, [this] {
        const QStringList files = QFileDialog::getOpenFileNames(this, QStringLiteral("Import to FujiNet's SD Card"),
                                                                mediaFolder(), QString::fromUtf8(kAnyFilter));
        int ok = 0, failed = 0;
        for (const QString &f : files) {
            char dest[1024];
            if (a52session_import_to_sd(m_session, f.toLocal8Bit().constData(), dest, sizeof dest) == 0)
                ok++;
            else
                failed++;
        }
        if (!files.isEmpty())
            toast(failed ? QStringLiteral("%1 copied to FujiNet's SD card, %2 could not be").arg(ok).arg(failed)
                         : QStringLiteral("%1 copied to FujiNet's SD card: boot it from CONFIG").arg(ok));
    });
    fuji->addAction(QStringLiteral("Open &SD Folder"), this, [this] {
        const QString dir = local(a52session_sd_path(m_session));
        QDir().mkpath(dir);
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    });

    QMenu *help = menuBar()->addMenu(QStringLiteral("&Help"));
    connect(appShortcut(this, help->addAction(QStringLiteral("FujiNet Go Atari 5200 &Help")),
                        QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_H)),
            &QAction::triggered, this, [this] {
        a52HelpFor(QApplication::activeWindow() ? QApplication::activeWindow() : this);
    });
    help->addSeparator();
    help->addAction(QStringLiteral("&About FujiNet Go Atari 5200"), this, &MainWindow::showAbout);
}

/* ---- media --------------------------------------------------------------------- */

QString MainWindow::mediaFolder() const
{
    const QString last = local(a52session_get_str(m_session, "last_dir", ""));
    if (!last.isEmpty() && QFileInfo(last).isDir())
        return last;
    return local(a52session_media_path(m_session));
}

void MainWindow::rememberFolder(const QString &path)
{
    a52session_set_str(m_session, "last_dir", QFileInfo(path).absolutePath().toLocal8Bit().constData());
}

void MainWindow::loadMedia(const QString &path)
{
    if (!a52session_is_running(m_session)) {
        toast(QStringLiteral("The console is off"));
        return;
    }
    char msg[1024] = "";
    const int rc = a52session_open_or_import(m_session, path.toLocal8Bit().constData(), msg, sizeof msg);
    if (rc < 0) {
        QMessageBox::warning(this, QStringLiteral("Cannot Open It"),
                             msg[0] ? local(msg) : local(a52session_last_error(m_session)));
        return;
    }
    if (rc != 0)
        rememberFolder(path);
    if (rc == A52_MEDIA_BIOS)
        QMessageBox::information(this, QStringLiteral("BIOS Imported"), local(msg));
    else
        toast(msg[0] ? local(msg) : QFileInfo(path).fileName());
    updateStatus();
}

void MainWindow::importBios(int mode)
{
    SettingsDialog::importBios(this, m_session, mode);
}

/* ---- the machine ------------------------------------------------------------------ */

void MainWindow::start()
{
    if (a52session_is_running(m_session))
        return;
    a52session_start_opts opts;
    a52session_default_opts(m_session, &opts);
    if (a52session_start(m_session, &opts) != 0)
        QMessageBox::warning(this, QStringLiteral("Cannot Start the Console"), local(a52session_last_error(m_session)));
    updateStatus();
}

void MainWindow::powerCycle()
{
    if (!a52session_is_running(m_session)) {
        start();
        return;
    }
    if (a52session_power_cycle(m_session) != 0)
        QMessageBox::warning(this, QStringLiteral("Cannot Power Cycle"), local(a52session_last_error(m_session)));
    updateStatus();
}

void MainWindow::runSysaction(int sa)
{
    switch (sa) {
    case A52_SYSACT_PAUSE: a52DebuggerShow(this, m_session); break;
    case A52_SYSACT_POWER: powerCycle(); break;
    case A52_SYSACT_WARP:
        a52session_set_warp(m_session, !a52session_warp(m_session));
        updateStatus();
        break;
    default: break;
    }
}

void MainWindow::showSettings()
{
    const int did = SettingsDialog::run(this, m_session, [this] { applyView(); });
    applyView();
    if (did == A52_APPLY_POWER)
        toast(QStringLiteral("Power cycled with the new configuration"));
    else if (did == A52_APPLY_RESTART)
        toast(QStringLiteral("Restarted with the new options"));
    updateStatus();
}

void MainWindow::showNotice(const QString &text)
{
    m_noticeOpen = true;
    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("The 5200 Stopped"));
    box.setText(QStringLiteral("<b>The 5200 Stopped</b>"));
    box.setInformativeText(text);
    QPushButton *power = box.addButton(QStringLiteral("Power Cycle"), QMessageBox::AcceptRole);
    QPushButton *dbg = box.addButton(QStringLiteral("Open the Debugger"), QMessageBox::ActionRole);
    box.addButton(QStringLiteral("Leave It"), QMessageBox::RejectRole);
    box.setDefaultButton(power);
    box.exec();
    m_noticeOpen = false;
    if (box.clickedButton() == power)
        powerCycle();
    else if (box.clickedButton() == dbg)
        a52DebuggerShow(this, m_session);
}

void MainWindow::showAbout()
{
    QMessageBox::about(this, QStringLiteral("About FujiNet Go Atari 5200"),
        QStringLiteral("<b>FujiNet Go Atari 5200</b> %1<br><br>"
                       "The Atari 5200 SuperSystem with the FujiNet cartridge in its slot.<br>"
                       "The emulator is atari800 (GPL-2.0-or-later, the Atari800 Development Team) "
                       "in 5200 mode, with the FujiNet cartridge's firmware (GPL-3.0-or-later) "
                       "connected over BoIP. The Altirra 5200 OS (Avery Lee) is built in; Atari's "
                       "own BIOS can be imported.<br><br>"
                       "Copyright © 2026 Thomas Cherryhomes — GPL-3.0-or-later<br>"
                       "<a href=\"https://fujinet.online/\">fujinet.online</a>")
            .arg(QStringLiteral(A52_VERSION_STRING)));
}

void MainWindow::useSoftwareDisplay()
{
    if (dynamic_cast<SoftDisplayWidget *>(m_display))
        return;
    auto *soft = new SoftDisplayWidget(m_session, this);
    m_display = soft;
    setCentralWidget(soft);   /* deletes the GPU display, which stops its feed */
    applyView();
    soft->setFocus();
}

/* ---- view, status --------------------------------------------------------------------- */

void MainWindow::applyView()
{
    const int i = a52session_pref_find("aspect");
    const bool tv = i < 0 || a52session_pref_get_choice(m_session, i) == 0;
    const bool smooth = a52session_get_int(m_session, "smooth", 0) != 0;
    m_display->setTvAspect(tv);
    m_display->setSmooth(smooth);
    if (m_tvAction) m_tvAction->setChecked(tv);
    if (m_smoothAction) m_smoothAction->setChecked(smooth);
}

void MainWindow::updateStatus()
{
    bool on = false;
    QString text = QStringLiteral("Off");
    if (a52session_is_running(m_session)) {
        char buf[512];
        a52session_status(m_session, buf, sizeof buf);
        text = local(buf);
        on = a52session_fujinet_link(m_session);
    }
    m_status->setText(text);
    m_dot->setStyleSheet(on ? QStringLiteral("color: %1;").arg(a52AccentColor().name())
                            : QStringLiteral("color: gray;"));
}

/* The gamepad thread cannot touch the UI; it posts system actions and this
 * timer takes them. It also says when gamepads come and go, when FujiNet
 * restarted itself, and when the Atari crashed with no debugger open. */
void MainWindow::fastTick()
{
    char text[512];
    int sa;
    while (a52session_sysaction_take(m_session, &sa))
        runSysaction(sa);
    unsigned gen = a52session_gamepad_generation(m_session);
    if (gen != m_padGeneration) {
        m_padGeneration = gen;
        if (a52session_gamepad_last_event(m_session, text, sizeof text) > 0)
            toast(local(text));
    }
    gen = a52session_fujinet_generation(m_session);
    if (gen != m_fujinetGeneration) {
        m_fujinetGeneration = gen;
        if (a52session_fujinet_last_event(m_session, text, sizeof text) > 0)
            toast(local(text));
        updateStatus();
    }
    if (!m_noticeOpen && a52session_take_notice(m_session, text, sizeof text) > 0) {
        const QString t = local(text);
        QTimer::singleShot(0, this, [this, t] { showNotice(t); });
    }
    if (!m_shotPath.isEmpty() && m_display->frames() >= m_shotFrames) {
        if (!m_display->savePng(m_shotPath))
            qWarning("A52_SCREENSHOT: could not write %s", qPrintable(m_shotPath));
        m_shotPath.clear();
    }
    if (m_quitFrames && m_display->frames() >= m_quitFrames) {
        m_quitFrames = 0;
        close();
    }
}

/* ---- events ------------------------------------------------------------------------------- */

void MainWindow::keyPressEvent(QKeyEvent *e)
{
    const int sa = a52KeySysaction(m_session, e);
    if (sa >= 0) {
        runSysaction(sa);
        return;
    }
    if (!a52ForwardKey(m_session, e, true))
        QMainWindow::keyPressEvent(e);
}

void MainWindow::keyReleaseEvent(QKeyEvent *e)
{
    if (!a52ForwardKey(m_session, e, false))
        QMainWindow::keyReleaseEvent(e);
}

/* Focus gone: nothing stays held. */
void MainWindow::changeEvent(QEvent *e)
{
    if (e->type() == QEvent::ActivationChange && !isActiveWindow() && !a52ControllersVisible())
        a52session_release_all(m_session);
    QMainWindow::changeEvent(e);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasUrls())
        e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *e)
{
    const QList<QUrl> urls = e->mimeData()->urls();
    if (urls.isEmpty())
        return;
    const QString path = urls.first().toLocalFile();
    if (!path.isEmpty())
        loadMedia(path);
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    m_statusTimer.stop();
    m_fastTimer.stop();
    QMainWindow::closeEvent(e);
}
