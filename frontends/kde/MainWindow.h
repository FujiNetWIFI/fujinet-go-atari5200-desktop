/*
 * The main window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QLabel>
#include <QMainWindow>
#include <QTimer>

#include "a52session.h"

class DisplayView;
class QAction;
class QMenu;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(a52session *session, QWidget *parent = nullptr);
    /* Starts the console (FujiNet first). */
    void start();
    /* A dropped or command-line file: Open's routing (a cartridge is served
     * by the FujiNet cartridge, a BIOS is imported, anything else goes to
     * the SD folder). */
    void loadMedia(const QString &path);

protected:
    void keyPressEvent(QKeyEvent *e) override;
    void keyReleaseEvent(QKeyEvent *e) override;
    void changeEvent(QEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;
    void closeEvent(QCloseEvent *e) override;

private:
    void buildMenus();
    void updateStatus();
    void fastTick();
    void applyView();
    void useSoftwareDisplay();
    void showSettings();
    void powerCycle();
    void runSysaction(int sa);
    void importBios(int mode);
    void showNotice(const QString &text);
    void showAbout();
    void toast(const QString &text, int ms = 4000);
    QString mediaFolder() const;
    void rememberFolder(const QString &path);

    a52session *m_session;
    DisplayView *m_display = nullptr;
    QMenu *m_machine = nullptr;
    QLabel *m_dot = nullptr;
    QLabel *m_status = nullptr;
    QTimer m_statusTimer;
    QTimer m_fastTimer;
    QAction *m_tvAction = nullptr;
    QAction *m_smoothAction = nullptr;
    QAction *m_warpAction = nullptr;
    QAction *m_pauseAction = nullptr;
    unsigned m_padGeneration = 0;
    unsigned m_fujinetGeneration = 0;
    bool m_noticeOpen = false;
    QString m_shotPath;
    quint64 m_shotFrames = 0, m_quitFrames = 0;
};
