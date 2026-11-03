/*
 * Debugger window (Qt6 Widgets) over the session's debugger engine and
 * atari800's own monitor, via core/include/a52debug.h. Mirrors the GNOME
 * one tab for tab.
 *
 * Tabs: the Console (atari800's monitor a line at a time, with Tab
 * completion and a command history; its A assembler prompts for lines),
 * CPU & Memory (registers, flags, the beam, a hex dump and the last
 * instructions run), the Disassembly (click a line to select it for Run to
 * Cursor; click its left edge, or double-click it, for its breakpoint),
 * ANTIC and its display list, GTIA (players, missiles, colours, collisions
 * and the palette), POKEY (with the controllers' pots and the keypad), the
 * Memory Map and the FujiNet cartridge's state, Breakpoints and watchpoints,
 * and FujiNet (every BoIP FujiBus packet the cartridge exchanges with
 * FujiNet, both ways, from session start, beside the cartridge's link and
 * mailbox state). The toolbar carries the stepping controls, symbol loading
 * and file saves; the family's keys apply (F5 run/stop, F7 step, F8 step
 * over, Shift+F8 step out, F9 frame, F12 close).
 *
 * Showing the window attaches (which stops the machine, as on every
 * sibling); hiding it detaches and lets the machine run on.
 *
 * A short timer polls the engine's generation and refreshes the tab showing
 * when it moved; while the machine runs, the status, the registers and the
 * FujiNet trace refresh twice a second. At each stop the console takes the
 * banner and registers atari800 printed, as its own monitor shows them.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "DebuggerWindow.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QShowEvent>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QSplitter>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextBlock>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidgetAction>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "../FujiNetWindows.h"
#include "a52debug.h"

#define DISASM_WINDOW 64        /* lines at most; as many show as fit */
#define MEM_ROWS 256            /* 4K of memory, 16 bytes a row */
#define HISTORY_MAX 64
#define FUJI_MAX 512
#define BP_MAX 64
#define CONSOLE_LINES 5000
#define SWATCHES 9
#define PAL_AXIS_W 30           /* the palette's hue labels */
#define PAL_AXIS_H 18           /* ... and its luma labels */
#define PAL_CELL_W 28
#define PAL_CELL_H 16

namespace {

enum { TAB_CONSOLE, TAB_CPU, TAB_DISASM, TAB_ANTIC, TAB_GTIA, TAB_POKEY, TAB_MAP,
       TAB_BREAKS, TAB_FUJINET, TAB_COUNT };

/* The tabs, in order: the names A52_DEBUGGER_TAB takes, and their labels
 * (which it takes too). */
const struct { const char *name, *label; } kTabs[TAB_COUNT] = {
    { "console", "Console" }, { "cpu", "CPU & Memory" }, { "disassembly", "Disassembly" },
    { "antic", "ANTIC" }, { "gtia", "GTIA" }, { "pokey", "POKEY" },
    { "map", "Memory Map" }, { "breakpoints", "Breakpoints" },
    { "fujinet", "FujiNet" },
};

const int kRegs[6] = { A52_REG_PC, A52_REG_SP, A52_REG_A, A52_REG_X, A52_REG_Y, A52_REG_P };
const int kFlags[6] = { A52_FLAG_N, A52_FLAG_V, A52_FLAG_D, A52_FLAG_I, A52_FLAG_Z, A52_FLAG_C };

void setMono(QWidget *w)
{
    QFont f = w->font();
    f.setFamily(QStringLiteral("monospace"));
    f.setStyleHint(QFont::TypeWriter);
    w->setFont(f);
}

QPlainTextEdit *monoView()
{
    auto *v = new QPlainTextEdit;
    v->setReadOnly(true);
    setMono(v);
    v->setLineWrapMode(QPlainTextEdit::NoWrap);
    return v;
}

/* Engine text for a view: control bytes dropped. */
QString cleanText(const char *s)
{
    const QString in = QString::fromUtf8(s);
    QString out;
    out.reserve(in.size());
    for (const QChar c : in)
        if (c.unicode() >= 0x20 || c == QLatin1Char('\n') || c == QLatin1Char('\t')) out += c;
    return out;
}

/* $0600, 0x0600, #1536; a bare number is hex, as everywhere in a 6502
 * debugger. */
bool parseNum(const QString &text, long *out)
{
    const QString s = text.trimmed();
    bool ok = false;
    long v;
    if (s.startsWith(QLatin1Char('$'))) v = s.mid(1).toLong(&ok, 16);
    else if (s.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) v = s.mid(2).toLong(&ok, 16);
    else if (s.startsWith(QLatin1Char('#'))) v = s.mid(1).toLong(&ok, 10);
    else v = s.toLong(&ok, 16);
    if (ok) *out = v;
    return ok;
}

Q_ATTRIBUTE_FORMAT_PRINTF(2, 3)
void appendf(QString &t, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    t += QString::vasprintf(fmt, ap);
    va_end(ap);
}

const char *onOff(int v) { return v ? "on" : "off"; }

QLabel *titleLabel(const QString &text)
{
    auto *l = new QLabel(text);
    QFont f = l->font();
    f.setBold(true);
    l->setFont(f);
    return l;
}

QLabel *hintLabel(const QString &text)
{
    auto *l = new QLabel(text);
    l->setStyleSheet(QStringLiteral("color: gray;"));
    l->setWordWrap(true);
    return l;
}

/* Replaces a view's text where the reader left it (a refresh must not throw
 * a scrolled view back to its top), or, following, at its end. */
void setText(QPlainTextEdit *v, const QString &text, bool follow = false)
{
    if (v->toPlainText() == text) {
        if (follow) v->verticalScrollBar()->setValue(v->verticalScrollBar()->maximum());
        return;
    }
    const int top = v->verticalScrollBar()->value(), left = v->horizontalScrollBar()->value();
    v->setPlainText(text);
    v->verticalScrollBar()->setValue(follow ? v->verticalScrollBar()->maximum() : top);
    v->horizontalScrollBar()->setValue(left);
}

/* Whether a view's reader is at its end (a log then follows new lines). */
bool atEnd(QPlainTextEdit *v)
{
    return v->verticalScrollBar()->value() >= v->verticalScrollBar()->maximum() - 1;
}

/* A whole line of a view in a format: the accent fill, a mark. */
QTextEdit::ExtraSelection lineSelection(QPlainTextEdit *v, int line, const QTextCharFormat &fmt)
{
    QTextEdit::ExtraSelection s;
    s.cursor = QTextCursor(v->document()->findBlockByNumber(line));
    s.format = fmt;
    s.format.setProperty(QTextFormat::FullWidthSelection, true);
    return s;
}

QTextCharFormat accentFormat()
{
    QTextCharFormat f;
    f.setBackground(a52AccentColor());
    f.setForeground(Qt::white);
    return f;
}

/* The status line, in the tab row's corner: as wide as its text, within
 * what the tabs leave, elided past that with the whole of it as the
 * tooltip, so a long stop reason never widens the window. */
class ElidedLabel : public QLabel {
public:
    void setFullText(const QString &text)
    {
        if (text == m_full) return;
        m_full = text;
        setToolTip(text);
        elide();
        updateGeometry();
    }
    QSize sizeHint() const override
    {
        const QMargins m = contentsMargins();
        int w = fontMetrics().horizontalAdvance(m_full) + m.left() + m.right() + 4;
        if (const auto *tabs = qobject_cast<const QTabWidget *>(parentWidget()))
            w = qMin(w, tabs->width() - tabs->tabBar()->sizeHint().width() - 16);
        return QSize(qMax(w, minimumSizeHint().width()), QLabel::sizeHint().height());
    }
    QSize minimumSizeHint() const override { return QSize(fontMetrics().averageCharWidth() * 8, 0); }

protected:
    void resizeEvent(QResizeEvent *e) override
    {
        QLabel::resizeEvent(e);
        elide();
    }

private:
    void elide()
    {
        const QMargins m = contentsMargins();
        setText(fontMetrics().elidedText(m_full, Qt::ElideRight, width() - m.left() - m.right()));
    }
    QString m_full;
};

/* The 256 colours, 16 hues down and 16 lumas across, nearest-neighbour so
 * the cells stay crisp, with their hex digits along the edges. A click
 * picks one. */
class PaletteView : public QWidget {
public:
    PaletteView()
    {
        setFixedSize(PAL_AXIS_W + 16 * PAL_CELL_W, PAL_AXIS_H + 16 * PAL_CELL_H);
        px.resize(A52DEBUG_PALETTE_WIDTH * A52DEBUG_PALETTE_HEIGHT);
    }
    std::vector<uint32_t> px;
    bool ok = false;
    std::function<void(int)> picked;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        QFont f = font();
        f.setFamily(QStringLiteral("monospace"));
        f.setStyleHint(QFont::TypeWriter);
        f.setPixelSize(11);
        p.setFont(f);
        p.setPen(palette().color(QPalette::WindowText));
        const double cw = (width() - PAL_AXIS_W) / 16.0, ch = (height() - PAL_AXIS_H) / 16.0;
        for (int i = 0; i < 16; ++i) {
            p.drawText(QRectF(PAL_AXIS_W + i * cw, 0, cw, PAL_AXIS_H), Qt::AlignCenter,
                       QString::asprintf("%X", i));
            p.drawText(QRectF(0, PAL_AXIS_H + i * ch, PAL_AXIS_W - 6, ch), Qt::AlignRight | Qt::AlignVCenter,
                       QString::asprintf("%X0", i));
        }
        if (!ok) return;
        const QImage img(reinterpret_cast<const uchar *>(px.data()), A52DEBUG_PALETTE_WIDTH,
                         A52DEBUG_PALETTE_HEIGHT, A52DEBUG_PALETTE_WIDTH * 4, QImage::Format_RGB32);
        p.drawImage(QRectF(PAL_AXIS_W, PAL_AXIS_H, width() - PAL_AXIS_W, height() - PAL_AXIS_H), img);
    }

    void mousePressEvent(QMouseEvent *e) override
    {
        const QPointF pos = e->position();
        if (pos.x() < PAL_AXIS_W || pos.y() < PAL_AXIS_H || !picked) return;
        const int luma = (int)((pos.x() - PAL_AXIS_W) * 16 / (width() - PAL_AXIS_W));
        const int hue = (int)((pos.y() - PAL_AXIS_H) * 16 / (height() - PAL_AXIS_H));
        if (luma > 15 || hue > 15) return;
        picked(hue << 4 | luma);
    }
};

class DebuggerWindow : public QMainWindow {
public:
    DebuggerWindow(a52session *session, QWidget *parent);
    ~DebuggerWindow() override;
    /* Attaches (stopping the machine) and refreshes. */
    void attachAndRefresh();

protected:
    bool event(QEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;
    bool eventFilter(QObject *obj, QEvent *e) override;

private:
    QWidget *buildToolbar();
    QWidget *buildConsole();
    QWidget *buildCpu();
    QWidget *buildDisasm();
    QWidget *buildAntic();
    QWidget *buildGtia();
    QWidget *buildBreaks();
    QWidget *buildFujinet();

    void refreshStatus();
    void refreshCpu();
    void refreshMem();
    void refreshHistory();
    void refreshDisasm();
    void refreshAntic();
    void refreshGtia();
    void refreshPokey();
    void refreshMap();
    void refreshBps();
    void refreshFujinet(bool force);
    void refreshPage(int page);
    void refreshAll();
    void sync();
    void syncLater();
    void settleAndSync();
    void tick();

    void consoleAppend(const QString &text);
    void consoleOutput(const char *text);
    void say(const QString &msg);
    void setPrompt(bool asmMode, const QString &prompt);
    void consoleDrain();
    void runConsole();
    void complete();
    void showHistory();

    void toggleRun();
    void stepAnd(void (*fn)(a52debug *));
    void runToCursor();
    void toggleBreakpoint(uint16_t addr);
    void addBreakpoint();
    void breakAtLine();
    void memGoto();
    void memWrite();
    void jumpTo();
    int visibleRows() const;
    void loadSymbols();
    void saveFile(const char *kind, const char *title, const char *file);
    bool addressOf(const QString &text, long *out);
    void message(const QString &msg);

    a52session *m_session;
    a52debug *m_dbg;
    unsigned m_seenGen = 0;
    bool m_wasStopped = false;
    bool m_attachedRunning = false;     /* attached while the machine was running */
    int m_runningTicks = 0;
    QTimer m_timer;

    QTabWidget *m_tabs = nullptr;
    ElidedLabel *m_status = nullptr;
    QPushButton *m_runBtn = nullptr;
    QPushButton *m_brkBtn = nullptr;

    /* console */
    QPlainTextEdit *m_conOut = nullptr;
    QLineEdit *m_conIn = nullptr;
    QLabel *m_conPrompt = nullptr;
    bool m_conAsm = false;              /* the A assembler wants its next line */
    QString m_conPromptText = QStringLiteral("> ");
    QStringList m_history;
    int m_historyPos = 0;

    /* cpu + memory */
    QLineEdit *m_reg[6] = {};           /* PC SP A X Y P */
    QCheckBox *m_flag[6] = {};          /* N V D I Z C */
    QLabel *m_beam = nullptr;
    QPlainTextEdit *m_mem = nullptr;
    QLineEdit *m_memGoto = nullptr, *m_memAddr = nullptr, *m_memVal = nullptr;
    int m_memTop = 0x0000;
    int m_memMark = -1;                 /* the address Go to found; -1 none */
    QPlainTextEdit *m_hist = nullptr;

    /* disassembly */
    QPlainTextEdit *m_disasm = nullptr;
    QCheckBox *m_followPc = nullptr;
    QLineEdit *m_jump = nullptr;
    int m_disasmTop = 0xE000;
    int m_selected = -1;                /* the selected line's address, or -1 */
    int m_wheel = 0;                    /* wheel travel not yet a row */
    std::vector<uint16_t> m_lineAddr;

    /* antic */
    QPlainTextEdit *m_antic = nullptr, *m_dlist = nullptr;

    /* gtia */
    QPlainTextEdit *m_gtia = nullptr;
    QLabel *m_swatch[SWATCHES] = {};
    QLabel *m_swatchLabel[SWATCHES] = {};
    uint32_t m_swatchRgb[SWATCHES];
    PaletteView *m_palette = nullptr;
    QLabel *m_paletteInfo = nullptr;

    /* pokey, the memory map */
    QPlainTextEdit *m_pokey = nullptr;
    QPlainTextEdit *m_map = nullptr;

    /* breakpoints */
    QTableWidget *m_bpTable = nullptr;
    QString m_bpSig;                    /* what the table shows, to rebuild on change */
    bool m_bpFilling = false;
    QLabel *m_bpEmpty = nullptr, *m_bpBudget = nullptr, *m_bpMsg = nullptr;
    QComboBox *m_bpType = nullptr;
    QLineEdit *m_bpStart = nullptr, *m_bpEnd = nullptr, *m_bpCond = nullptr, *m_bpLine = nullptr;

    /* fujinet */
    QPlainTextEdit *m_fujiText = nullptr, *m_fujiState = nullptr;
    QLabel *m_fujiNote = nullptr;
    int m_fujiCount = -1;
    uint64_t m_fujiLast = 0;
};

QPointer<DebuggerWindow> s_win;

/* A52_DEBUGGER_TAB: a page number, or a tab's name or label (any case), or
 * the start of one, or of a word of a label ("map", "fujinet" ...). */
int tabFromEnv()
{
    const QString tab = qEnvironmentVariable("A52_DEBUGGER_TAB").trimmed();
    if (tab.isEmpty()) return -1;
    bool isNum = false;
    const int n = tab.toInt(&isNum);
    if (isNum) return n >= 0 && n < TAB_COUNT ? n : -1;
    for (int pass = 0; pass < 3; ++pass)
        for (int i = 0; i < TAB_COUNT; ++i) {
            const QString name = QString::fromUtf8(kTabs[i].name), label = QString::fromUtf8(kTabs[i].label);
            if (pass == 0 && name.startsWith(tab, Qt::CaseInsensitive)) return i;
            if (pass == 1 && label.startsWith(tab, Qt::CaseInsensitive)) return i;
            if (pass == 2) {
                const QStringList words = label.split(QLatin1Char(' '));
                for (int w = 1; w < words.size(); ++w)
                    if (words[w].startsWith(tab, Qt::CaseInsensitive)) return i;
            }
        }
    return -1;
}

DebuggerWindow::DebuggerWindow(a52session *session, QWidget *parent)
    : QMainWindow(parent, Qt::Window), m_session(session), m_dbg(a52session_debugger(session))
{
    setObjectName(QStringLiteral("debugger"));
    setWindowTitle(QStringLiteral("Debugger"));
    resize(1250, 800);          /* the toolbar at its full width */
    memset(m_swatchRgb, 0xFF, sizeof m_swatchRgb);

    auto *central = new QWidget;
    auto *root = new QVBoxLayout(central);
    root->addWidget(buildToolbar());
    m_tabs = new QTabWidget;
    m_tabs->setUsesScrollButtons(true);
    QWidget *pages[TAB_COUNT];
    pages[TAB_CONSOLE] = buildConsole();
    pages[TAB_CPU] = buildCpu();
    pages[TAB_DISASM] = buildDisasm();
    pages[TAB_ANTIC] = buildAntic();
    pages[TAB_GTIA] = buildGtia();
    pages[TAB_POKEY] = m_pokey = monoView();
    pages[TAB_MAP] = m_map = monoView();
    pages[TAB_BREAKS] = buildBreaks();
    pages[TAB_FUJINET] = buildFujinet();
    for (int i = 0; i < TAB_COUNT; ++i)
        m_tabs->addTab(pages[i], QString::fromUtf8(kTabs[i].label).replace(QLatin1Char('&'), QLatin1String("&&")));
    /* the stop reason at the end of the tab row (the toolbar row is full) */
    m_status = new ElidedLabel;
    m_status->setStyleSheet(QStringLiteral("color: gray;"));
    m_status->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_status->setContentsMargins(8, 0, 8, 0);
    m_tabs->setCornerWidget(m_status, Qt::TopRightCorner);
    const int tab = tabFromEnv();
    if (tab >= 0) m_tabs->setCurrentIndex(tab);
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int page) { refreshPage(page); });
    root->addWidget(m_tabs, 1);
    setCentralWidget(central);
    statusBar();

    connect(&m_timer, &QTimer::timeout, this, [this] { tick(); });
    /* the refresh timer runs only while the window is shown (showEvent) */
}

/* Gone with the main window (the session outlives it): the machine runs on. */
DebuggerWindow::~DebuggerWindow()
{
    m_timer.stop();
    a52debug_detach(m_dbg);
}

/* ---- building ------------------------------------------------------------- */

QWidget *DebuggerWindow::buildToolbar()
{
    auto *bar = new QWidget;
    auto *h = new QHBoxLayout(bar);
    h->setContentsMargins(0, 0, 0, 0);
    auto add = [&](const QString &label, const QString &tip, std::function<void()> fn) {
        auto *b = new QPushButton(label);
        b->setFocusPolicy(Qt::NoFocus);
        b->setToolTip(tip);
        connect(b, &QPushButton::clicked, this, fn);
        h->addWidget(b);
        return b;
    };
    m_runBtn = add(QStringLiteral("Stop (F5)"), QStringLiteral("Run, or stop before the next instruction"),
                   [this] { toggleRun(); });
    add(QStringLiteral("Step (F7)"), QStringLiteral("One instruction"), [this] { stepAnd(a52debug_step); });
    add(QStringLiteral("Step Over (F8)"), QStringLiteral("Over a JSR, to the instruction after it"),
        [this] { stepAnd(a52debug_step_over); });
    add(QStringLiteral("Step Out (⇧F8)"), QStringLiteral("On to the RTS or RTI"),
        [this] { stepAnd(a52debug_step_out); });
    add(QStringLiteral("Frame (F9)"), QStringLiteral("On to the next vertical blank"),
        [this] { stepAnd(a52debug_frame); });
    add(QStringLiteral("Run to Cursor"), QStringLiteral("On to the line selected in the Disassembly"),
        [this] { runToCursor(); });
    m_brkBtn = new QPushButton(QStringLiteral("Break on BRK"));
    m_brkBtn->setFocusPolicy(Qt::NoFocus);
    m_brkBtn->setCheckable(true);
    m_brkBtn->setToolTip(QStringLiteral("Stop at a BRK instead of running it"));
    connect(m_brkBtn, &QPushButton::clicked, this, [this](bool on) {
        a52debug_break_on_brk(m_dbg, on);
        sync();
    });
    h->addWidget(m_brkBtn);
    h->addStretch(1);

    auto *sym = new QPushButton(QStringLiteral("Load Symbols…"));
    sym->setFocusPolicy(Qt::NoFocus);
    sym->setToolTip(QStringLiteral("Labels from ld65 -Ln, VICE .lbl, MADS .lab or an atari800 "
                                   "label file, beside the OS's own"));
    connect(sym, &QPushButton::clicked, this, [this] { loadSymbols(); });
    h->addWidget(sym);

    auto *save = new QPushButton(QStringLiteral("Save…"));
    save->setFocusPolicy(Qt::NoFocus);
    auto *menu = new QMenu(save);
    static const struct { const char *kind, *title, *file; } saves[] = {
        { "dis", "Disassembly (256 lines from the PC)", "disassembly.asm" },
        { "ram", "Memory (64K as the CPU sees it)", "memory.bin" } };
    for (const auto &s : saves) {
        const char *kind = s.kind, *title = s.title, *file = s.file;
        menu->addAction(QString::fromUtf8(title), this, [this, kind, title, file] { saveFile(kind, title, file); });
    }
    save->setMenu(menu);
    h->addWidget(save);
    return bar;
}

QWidget *DebuggerWindow::buildConsole()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    m_conOut = monoView();
    m_conOut->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_conOut->setMaximumBlockCount(CONSOLE_LINES);
    m_conOut->setPlainText(QStringLiteral("atari800's monitor (5200 mode). Type HELP for its commands; Tab completes them, "
                                          "Up/Down recall. G, R, O and CONT let the machine go; A assembles "
                                          "a line at a time.\n"));
    auto *row = new QHBoxLayout;
    m_conPrompt = new QLabel(m_conPromptText);
    setMono(m_conPrompt);
    m_conIn = new QLineEdit;
    setMono(m_conIn);
    m_conIn->installEventFilter(this);
    connect(m_conIn, &QLineEdit::returnPressed, this, [this] { runConsole(); });
    row->addWidget(m_conPrompt);
    row->addWidget(m_conIn, 1);
    v->addWidget(m_conOut, 1);
    v->addLayout(row);
    setPrompt(false, QString());
    return w;
}

QWidget *DebuggerWindow::buildCpu()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    static const char *const names[6] = { "PC", "SP", "A", "X", "Y", "P" };
    static const char *const fnames[6] = { "N", "V", "D", "I", "Z", "C" };

    auto *regs = new QHBoxLayout;
    for (int i = 0; i < 6; ++i) {
        auto *l = new QLabel(QString::fromUtf8(names[i]));
        l->setStyleSheet(QStringLiteral("color: gray;"));
        m_reg[i] = new QLineEdit;
        setMono(m_reg[i]);
        m_reg[i]->setMaxLength(6);
        m_reg[i]->setFixedWidth(m_reg[i]->fontMetrics().horizontalAdvance(QStringLiteral("$FFFF")) + 16);
        m_reg[i]->setToolTip(QStringLiteral("Enter sets it"));
        connect(m_reg[i], &QLineEdit::returnPressed, this, [this, i] {
            long val;
            if (parseNum(m_reg[i]->text(), &val))
                a52debug_cpu_set(m_dbg, kRegs[i], (int)(val & (i == 0 ? 0xFFFF : 0xFF)));
            else
                message(QStringLiteral("Not a number ($ or plain hex, # decimal)"));
            sync();
        });
        regs->addWidget(l);
        regs->addWidget(m_reg[i]);
        regs->addSpacing(6);
    }
    regs->addStretch();
    v->addLayout(regs);

    auto *flags = new QHBoxLayout;
    for (int i = 0; i < 6; ++i) {
        m_flag[i] = new QCheckBox(QString::fromUtf8(fnames[i]));
        connect(m_flag[i], &QCheckBox::clicked, this, [this, i](bool on) {
            if (a52debug_is_stopped(m_dbg)) a52debug_cpu_set(m_dbg, kFlags[i], on);
            sync();
        });
        flags->addWidget(m_flag[i]);
    }
    flags->addSpacing(16);
    m_beam = new QLabel;
    m_beam->setStyleSheet(QStringLiteral("color: gray;"));
    m_beam->setTextInteractionFlags(Qt::TextSelectableByMouse);
    flags->addWidget(m_beam, 1);
    v->addLayout(flags);

    /* the dump beside the last instructions */
    auto *split = new QSplitter(Qt::Horizontal);
    auto *left = new QWidget;
    auto *lv = new QVBoxLayout(left);
    lv->setContentsMargins(0, 0, 0, 0);
    lv->addWidget(titleLabel(QStringLiteral("Memory, as the CPU sees it (reads have no side effects)")));
    m_mem = monoView();
    lv->addWidget(m_mem, 1);
    left->setMinimumWidth(560);
    auto *right = new QWidget;
    auto *rv = new QVBoxLayout(right);
    rv->setContentsMargins(0, 0, 0, 0);
    rv->addWidget(titleLabel(QStringLiteral("Last instructions")));
    m_hist = monoView();
    rv->addWidget(m_hist, 1);
    right->setMinimumWidth(280);
    split->addWidget(left);
    split->addWidget(right);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    split->setChildrenCollapsible(false);
    v->addWidget(split, 1);

    /* Go to, a page back or on, and a write */
    auto *nav = new QHBoxLayout;
    nav->addWidget(new QLabel(QStringLiteral("Go to")));
    m_memGoto = new QLineEdit;
    m_memGoto->setPlaceholderText(QStringLiteral("$0600 / label"));
    m_memGoto->setMaximumWidth(130);
    connect(m_memGoto, &QLineEdit::returnPressed, this, [this] { memGoto(); });
    nav->addWidget(m_memGoto);
    for (int dir = -1; dir <= 1; dir += 2) {
        auto *b = new QToolButton;
        b->setArrowType(dir < 0 ? Qt::UpArrow : Qt::DownArrow);
        b->setToolTip(dir < 0 ? QStringLiteral("Back 4K") : QStringLiteral("On 4K"));
        connect(b, &QToolButton::clicked, this, [this, dir] {
            m_memTop = (m_memTop + dir * MEM_ROWS * 16) & 0xFFFF;
            refreshMem();
        });
        nav->addWidget(b);
    }
    nav->addSpacing(12);
    nav->addWidget(new QLabel(QStringLiteral("Write")));
    m_memAddr = new QLineEdit;
    m_memAddr->setPlaceholderText(QStringLiteral("$0600"));
    m_memAddr->setMaximumWidth(110);
    m_memVal = new QLineEdit;
    m_memVal->setPlaceholderText(QStringLiteral("$00"));
    m_memVal->setMaximumWidth(56);
    connect(m_memAddr, &QLineEdit::returnPressed, this, [this] { memWrite(); });
    connect(m_memVal, &QLineEdit::returnPressed, this, [this] { memWrite(); });
    nav->addWidget(m_memAddr);
    nav->addWidget(new QLabel(QStringLiteral("value")));
    nav->addWidget(m_memVal);
    nav->addWidget(hintLabel(QStringLiteral("a CPU write: hardware registers take it as the 6502's would")), 1);
    v->addLayout(nav);
    return w;
}

QWidget *DebuggerWindow::buildDisasm()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *row = new QHBoxLayout;
    m_followPc = new QCheckBox(QStringLiteral("Follow PC"));
    m_followPc->setChecked(true);
    connect(m_followPc, &QCheckBox::clicked, this, [this] { refreshDisasm(); });
    m_jump = new QLineEdit;
    m_jump->setPlaceholderText(QStringLiteral("$E456 or CIOV"));
    m_jump->setMaximumWidth(150);
    connect(m_jump, &QLineEdit::returnPressed, this, [this] { jumpTo(); });
    row->addWidget(m_followPc);
    row->addSpacing(8);
    row->addWidget(new QLabel(QStringLiteral("Jump to")));
    row->addWidget(m_jump);
    row->addSpacing(8);
    row->addWidget(hintLabel(QStringLiteral("Click a line to select it (Run to Cursor); click its left edge or "
                                            "double-click it for a breakpoint (* on, o off); scroll to browse")),
                   1);
    v->addLayout(row);
    m_disasm = monoView();
    /* no scrollbar: the wheel scrolls by instructions, as many lines as fit */
    m_disasm->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_disasm->setFocusPolicy(Qt::NoFocus);
    m_disasm->viewport()->setCursor(Qt::ArrowCursor);
    m_disasm->viewport()->installEventFilter(this);
    v->addWidget(m_disasm, 1);
    return w;
}

QWidget *DebuggerWindow::buildAntic()
{
    auto *split = new QSplitter(Qt::Horizontal);
    auto *left = new QWidget;
    auto *lv = new QVBoxLayout(left);
    lv->addWidget(titleLabel(QStringLiteral("Registers")));
    m_antic = monoView();
    lv->addWidget(m_antic, 1);
    left->setMinimumWidth(520);
    auto *right = new QWidget;
    auto *rv = new QVBoxLayout(right);
    rv->addWidget(titleLabel(QStringLiteral("Display list")));
    m_dlist = monoView();
    rv->addWidget(m_dlist, 1);
    right->setMinimumWidth(240);
    split->addWidget(left);
    split->addWidget(right);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    split->setChildrenCollapsible(false);
    return split;
}

QWidget *DebuggerWindow::buildGtia()
{
    auto *w = new QWidget;
    auto *h = new QHBoxLayout(w);
    m_gtia = monoView();
    h->addWidget(m_gtia, 1);

    auto *right = new QVBoxLayout;
    right->addWidget(titleLabel(QStringLiteral("Colour registers")));
    /* COLPM0-3, COLPF0-3, then COLBK */
    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(12);
    for (int i = 0; i < SWATCHES; ++i) {
        auto *cell = new QHBoxLayout;
        cell->setSpacing(6);
        m_swatch[i] = new QLabel;
        m_swatch[i]->setFixedSize(22, 18);
        m_swatchLabel[i] = new QLabel;
        setMono(m_swatchLabel[i]);
        cell->addWidget(m_swatch[i]);
        cell->addWidget(m_swatchLabel[i]);
        cell->addStretch();
        grid->addLayout(cell, i / 4, i & 3);
    }
    right->addLayout(grid);
    right->addSpacing(8);
    right->addWidget(titleLabel(QStringLiteral("Palette: hues down, lumas across")));
    m_palette = new PaletteView;
    m_palette->picked = [this](int index) {
        m_paletteInfo->setText(QString::asprintf("Colour $%02X: hue %d, luma %d, #%06X", index, index >> 4,
                                                 index & 15, a52debug_color(m_dbg, (uint8_t)index) & 0xFFFFFF));
    };
    right->addWidget(m_palette);
    m_paletteInfo = hintLabel(QStringLiteral("Click a colour for its value"));
    m_paletteInfo->setTextInteractionFlags(Qt::TextSelectableByMouse);
    right->addWidget(m_paletteInfo);
    right->addStretch();
    h->addLayout(right);
    return w;
}

QWidget *DebuggerWindow::buildBreaks()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);

    auto *top = new QHBoxLayout;
    m_bpBudget = hintLabel(QString());
    m_bpBudget->setToolTip(QStringLiteral("atari800 checks up to 20 entries before each instruction: a breakpoint "
                                          "takes one (two for a range), each condition one more, and one joins "
                                          "each to the next"));
    auto *clear = new QPushButton(QStringLiteral("Clear All"));
    connect(clear, &QPushButton::clicked, this, [this] {
        a52debug_breakpoint_clear(m_dbg);
        sync();
    });
    top->addWidget(m_bpBudget, 1);
    top->addWidget(clear);
    v->addLayout(top);

    m_bpTable = new QTableWidget(0, 6);
    m_bpTable->setHorizontalHeaderLabels({ QStringLiteral("On"), QStringLiteral("ID"), QStringLiteral("Type"),
                                           QStringLiteral("Address"), QStringLiteral("Condition"), QString() });
    m_bpTable->verticalHeader()->setVisible(false);
    m_bpTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_bpTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    m_bpTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_bpTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_bpTable->setFocusPolicy(Qt::NoFocus);
    connect(m_bpTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        if (m_bpFilling || item->column() != 0) return;
        a52debug_breakpoint_enable(m_dbg, item->data(Qt::UserRole).toInt(), item->checkState() == Qt::Checked);
        syncLater();
    });
    v->addWidget(m_bpTable, 1);
    m_bpEmpty = hintLabel(QStringLiteral("No breakpoints or watchpoints. Click the left edge of a disassembly "
                                         "line, or add one below."));
    v->addWidget(m_bpEmpty);

    auto *row = new QHBoxLayout;
    m_bpType = new QComboBox;
    m_bpType->addItems({ QStringLiteral("Execute"), QStringLiteral("Read"), QStringLiteral("Write"),
                         QStringLiteral("Read/Write") });
    m_bpStart = new QLineEdit;
    m_bpStart->setPlaceholderText(QStringLiteral("$0600 / label"));
    m_bpStart->setMaximumWidth(130);
    m_bpEnd = new QLineEdit;
    m_bpEnd->setPlaceholderText(QStringLiteral("(end)"));
    m_bpEnd->setMaximumWidth(90);
    m_bpCond = new QLineEdit;
    m_bpCond->setPlaceholderText(QStringLiteral("condition: A=$10 && X<4 && MEM[$D01F]=6 — optional"));
    auto *add = new QPushButton(QStringLiteral("Add"));
    connect(add, &QPushButton::clicked, this, [this] { addBreakpoint(); });
    for (QLineEdit *e : { m_bpStart, m_bpEnd, m_bpCond })
        connect(e, &QLineEdit::returnPressed, this, [this] { addBreakpoint(); });
    row->addWidget(m_bpType);
    row->addWidget(m_bpStart);
    row->addWidget(m_bpEnd);
    row->addWidget(m_bpCond, 1);
    row->addWidget(add);
    v->addLayout(row);

    auto *line = new QHBoxLayout;
    line->addWidget(new QLabel(QStringLiteral("Break at scanline")));
    m_bpLine = new QLineEdit;
    m_bpLine->setPlaceholderText(QStringLiteral("100"));
    m_bpLine->setMaximumWidth(60);
    connect(m_bpLine, &QLineEdit::returnPressed, this, [this] { breakAtLine(); });
    auto *set = new QPushButton(QStringLiteral("Set"));
    connect(set, &QPushButton::clicked, this, [this] { breakAtLine(); });
    line->addWidget(m_bpLine);
    line->addWidget(set);
    line->addWidget(hintLabel(QStringLiteral("atari800's BLINE: the next Run (or step) stops when the beam "
                                             "reaches it, once")), 1);
    v->addLayout(line);

    m_bpMsg = new QLabel;
    m_bpMsg->setWordWrap(true);
    m_bpMsg->setStyleSheet(QStringLiteral("color: %1;").arg(a52AccentTextColor(this).name()));
    v->addWidget(m_bpMsg);
    return w;
}

QWidget *DebuggerWindow::buildFujinet()
{
    auto *split = new QSplitter(Qt::Horizontal);
    auto *left = new QWidget;
    auto *lv = new QVBoxLayout(left);
    lv->setContentsMargins(0, 0, 0, 0);
    lv->addWidget(titleLabel(QStringLiteral("FujiBus packets over BoIP")));
    auto *row = new QHBoxLayout;
    m_fujiNote = hintLabel(QString());
    auto *clear = new QPushButton(QStringLiteral("Clear"));
    connect(clear, &QPushButton::clicked, this, [this] {
        a52debug_fujinet_trace_clear(m_dbg);
        refreshFujinet(true);
    });
    row->addWidget(m_fujiNote, 1);
    row->addWidget(clear);
    lv->addLayout(row);
    m_fujiText = monoView();
    lv->addWidget(m_fujiText, 1);
    left->setMinimumWidth(560);

    auto *right = new QWidget;
    auto *rv = new QVBoxLayout(right);
    rv->setContentsMargins(0, 0, 0, 0);
    rv->addWidget(titleLabel(QStringLiteral("The cartridge's link and mailbox")));
    m_fujiState = monoView();
    rv->addWidget(m_fujiState, 1);
    right->setMinimumWidth(320);
    split->addWidget(left);
    split->addWidget(right);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    split->setChildrenCollapsible(false);
    return split;
}

/* ---- refresh -------------------------------------------------------------- */

void DebuggerWindow::refreshStatus()
{
    char reason[160];
    int addr = -1;
    const bool running = a52session_is_running(m_session) != 0;
    const bool stopped = running && a52debug_is_stopped(m_dbg);
    QString text;
    if (!running) {
        text = QStringLiteral("The console is not running");
    } else if (stopped) {
        a52debug_stop_reason(m_dbg, reason, sizeof reason, &addr);
        text = QStringLiteral("Stopped%1%2").arg(reason[0] ? QStringLiteral(": ") : QString(), QString::fromUtf8(reason));
    } else {
        text = QStringLiteral("Running");
    }
    m_status->setFullText(text);
    m_runBtn->setText(stopped ? QStringLiteral("Run (F5)") : QStringLiteral("Stop (F5)"));
    m_runBtn->setStyleSheet(stopped ? QStringLiteral("background: %1; color: white;").arg(a52AccentColor().name())
                                    : QString());
    m_brkBtn->setChecked(running && a52debug_breaks_on_brk(m_dbg));
}

void DebuggerWindow::refreshCpu()
{
    a52debug_cpu c;
    a52debug_cpu_get(m_dbg, &c);
    /* a value being typed stays while the machine runs */
    const bool running = !a52debug_is_stopped(m_dbg);
    const int vals[6] = { c.pc, c.sp, c.a, c.x, c.y, c.p };
    for (int i = 0; i < 6; ++i)
        if (!(running && m_reg[i]->hasFocus()))
            m_reg[i]->setText(i == 0 ? QString::asprintf("%04X", (unsigned)vals[i] & 0xFFFF)
                                     : QString::asprintf("%02X", (unsigned)vals[i] & 0xFF));
    const int flags[6] = { c.n, c.v, c.d, c.i, c.z, c.c };
    for (int i = 0; i < 6; ++i) m_flag[i]->setChecked(flags[i] != 0);
    m_beam->setText(QString::asprintf("scanline %d   cycle %d   frame %u", c.scanline, c.cycle, c.frame));
}

void DebuggerWindow::refreshMem()
{
    static uint8_t mem[MEM_ROWS * 16];
    a52debug_read(m_dbg, (uint16_t)m_memTop, mem, (int)sizeof mem);
    QString t;
    t.reserve(MEM_ROWS * 80);
    t += QStringLiteral("       ");
    for (int col = 0; col < 16; ++col) appendf(t, col == 8 ? " %X  " : "%X  ", col);
    t += QLatin1Char('\n');
    for (int row = 0; row < MEM_ROWS; ++row) {
        const uint8_t *r = mem + row * 16;
        appendf(t, "$%04X: ", (m_memTop + row * 16) & 0xFFFF);
        for (int col = 0; col < 16; ++col) appendf(t, col == 8 ? " %02X " : "%02X ", r[col]);
        t += QLatin1Char(' ');
        for (int col = 0; col < 16; ++col) t += QLatin1Char(r[col] >= 0x20 && r[col] < 0x7F ? (char)r[col] : '.');
        if (row < MEM_ROWS - 1) t += QLatin1Char('\n');
    }
    setText(m_mem, t);

    /* the row Go to found stays marked across refreshes */
    QList<QTextEdit::ExtraSelection> sel;
    if (m_memMark >= 0) {
        const int off = (m_memMark - m_memTop) & 0xFFFF;
        if (off < MEM_ROWS * 16) sel << lineSelection(m_mem, 1 + off / 16, accentFormat());
    }
    m_mem->setExtraSelections(sel);
}

void DebuggerWindow::refreshHistory()
{
    static a52debug_history h[HISTORY_MAX];
    const int n = a52debug_history_get(m_dbg, h, HISTORY_MAX);
    QString t = QStringLiteral("PC    line  cyc  instruction");
    for (int i = 0; i < n; ++i)
        appendf(t, "\n%04X   %3d  %3d  %s", h[i].pc, h[i].scanline, h[i].cycle, h[i].disasm);
    setText(m_hist, cleanText(t.toUtf8().constData()), true);
}

/* The disassembly lines that fit the view. */
int DebuggerWindow::visibleRows() const
{
    const int line = m_disasm->fontMetrics().lineSpacing();
    const int height = m_disasm->viewport()->height() - 2 * (int)m_disasm->document()->documentMargin();
    if (line <= 0 || height <= 0) return 32;
    return qBound(4, height / line, DISASM_WINDOW);
}

void DebuggerWindow::refreshDisasm()
{
    static a52debug_line lines[DISASM_WINDOW];
    int bp[DISASM_WINDOW];
    const int rows = visibleRows();

    /* Following, the PC stays put while it shows, else sits a third of the
     * way down. */
    if (m_followPc->isChecked()) {
        a52debug_cpu c;
        a52debug_cpu_get(m_dbg, &c);
        bool shows = false;
        for (int i = 1; i < (int)m_lineAddr.size() && i < rows - 2; ++i)
            if (m_lineAddr[i] == c.pc) shows = true;
        if (!shows) m_disasmTop = a52debug_row_address(m_dbg, (uint16_t)c.pc, -(rows / 3));
    }

    int pcLine = -1;
    const int n = a52debug_disassemble(m_dbg, (uint16_t)m_disasmTop, lines, qMin(rows + 1, DISASM_WINDOW), &pcLine);
    /* the label column is as wide as the longest label showing, within reason */
    int labelWidth = 0;
    for (int i = 0; i < n; ++i) labelWidth = qMax(labelWidth, (int)strlen(lines[i].label));
    labelWidth = qMin(labelWidth, 16);
    m_lineAddr.clear();
    QString t;
    for (int i = 0; i < n; ++i) {
        const a52debug_line *l = &lines[i];
        bp[i] = l->has_breakpoint ? a52debug_breakpoint_check(m_dbg, l->address) : 0;
        m_lineAddr.push_back(l->address);
        appendf(t, "%c%c %04X  %-8s  ", bp[i] == 2 ? 'o' : bp[i] ? '*' : ' ', l->is_pc ? '>' : ' ', l->address,
                l->bytes);
        if (labelWidth) appendf(t, "%-*.*s ", labelWidth, labelWidth, l->label);
        appendf(t, "%-16s%s%s", l->disasm, l->comment[0] ? " ; " : "", l->comment);
        if (i < n - 1) t += QLatin1Char('\n');
    }
    if (n == 0) t = QStringLiteral("(no disassembly)");
    m_disasm->setPlainText(cleanText(t.toUtf8().constData()));
    m_disasm->verticalScrollBar()->setValue(0);

    QList<QTextEdit::ExtraSelection> sel;
    for (int i = 0; i < n; ++i) {
        /* the gutter: a breakpoint red, a disabled one grey */
        if (bp[i]) {
            QTextEdit::ExtraSelection s;
            s.cursor = QTextCursor(m_disasm->document()->findBlockByNumber(i));
            s.cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
            if (bp[i] == 2) {
                s.format.setForeground(QColor(0x9a, 0x99, 0x96));
            } else {
                s.format.setForeground(QColor(0xe0, 0x1b, 0x24));
                s.format.setFontWeight(QFont::Bold);
            }
            sel << s;
        }
        /* the selected line underlined */
        if (lines[i].address == m_selected && i != pcLine) {
            QTextEdit::ExtraSelection s;
            s.cursor = QTextCursor(m_disasm->document()->findBlockByNumber(i));
            s.cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
            s.format.setFontUnderline(true);
            s.format.setFontWeight(QFont::Bold);
            sel << s;
        }
    }
    /* the PC line in the accent colour, last so its white text wins */
    if (pcLine >= 0 && pcLine < n) {
        QTextCharFormat f = accentFormat();
        if (lines[pcLine].address == m_selected) f.setFontUnderline(true);
        sel << lineSelection(m_disasm, pcLine, f);
    }
    m_disasm->setExtraSelections(sel);
}

void DebuggerWindow::refreshAntic()
{
    static const char *const widths[4] = {
        "none", "narrow (32 bytes a line)", "normal (40 bytes a line)", "wide (48 bytes a line)" };
    static char dl[32768];
    a52debug_antic a;
    a52debug_antic_get(m_dbg, &a);
    const int pm = a.pm_single_line ? (a.pmbase & 0xF8) << 8 : (a.pmbase & 0xFC) << 8;
    QString t;
    appendf(t,
        "DMACTL  $%02X   playfield %s\n"
        "              display list DMA %s\n"
        "              players %s, missiles %s, %s-line resolution\n"
        "CHACTL  $%02X   inverse characters %s%s%s\n"
        "HSCROL  $%02X\n"
        "VSCROL  $%02X\n"
        "PMBASE  $%02X   players and missiles at $%04X\n"
        "CHBASE  $%02X   character set at $%04X\n"
        "NMIEN   $%02X   display list interrupts %s, vertical blank %s\n"
        "NMIST   $%02X  %s%s%s%s\n"
        "DLIST   $%04X (the list this frame began with)\n\n"
        "Beam    scanline %d, cycle %d, VCOUNT $%02X\n",
        a.dmactl, widths[a.playfield_width & 3], onOff(a.dl_dma),
        onOff(a.pm_dma), onOff(a.missile_dma), a.pm_single_line ? "single" : "double",
        a.chactl, (a.chactl & 2) ? "inverse" : "normal",
        (a.chactl & 1) ? ", blanked" : "", (a.chactl & 4) ? ", upside down" : "",
        a.hscrol, a.vscrol, a.pmbase, pm, a.chbase, a.chbase << 8,
        a.nmien, onOff(a.nmien & 0x80), onOff(a.nmien & 0x40),
        a.nmist, (a.nmist & 0x80) ? " DLI" : "", (a.nmist & 0x40) ? " VBI" : "",
        (a.nmist & 0x20) ? " RESET" : "", (a.nmist & 0xE0) ? " (last interrupts)" : " -",
        a.dlist, a.scanline, a.cycle, a.vcount & 0xFF);
    setText(m_antic, t);

    QString d;
    if (a52debug_dlist_text(m_dbg, dl, sizeof dl) <= 0)
        d = QStringLiteral("(no display list)");
    else if (!a.dl_dma)
        d = QStringLiteral("(display list DMA is off: ANTIC is not reading this)\n\n") + cleanText(dl);
    else
        d = cleanText(dl);
    setText(m_dlist, d);
}

/* A GRAF byte's bits, left to right. */
void grafBits(QString &t, unsigned v, int bits)
{
    for (int b = bits - 1; b >= 0; --b) t += QLatin1Char((v >> b) & 1 ? '#' : '.');
}

void collisionRow(QString &t, const char *name, uint8_t pf, uint8_t pl, int self)
{
    appendf(t, "  %s        ", name);
    for (int j = 0; j < 4; ++j) appendf(t, " %c  ", (pf >> j) & 1 ? 'X' : '.');
    t += QStringLiteral("   ");
    for (int j = 0; j < 4; ++j) appendf(t, "%c  ", j == self ? '-' : (pl >> j) & 1 ? 'X' : '.');
    t += QLatin1Char('\n');
}

void DebuggerWindow::refreshGtia()
{
    static const char *const sizes[4] = { "normal", "double", "normal", "quad" };
    static const char *const modes[4] = {
        "normal (ANTIC's colours)", "9 (16 lumas of one hue)", "10 (9 colours)", "11 (16 hues)" };
    static const char *const names[SWATCHES] = {
        "COLPM0", "COLPM1", "COLPM2", "COLPM3", "COLPF0", "COLPF1", "COLPF2", "COLPF3", "COLBK" };
    a52debug_gtia g;
    a52debug_gtia_get(m_dbg, &g);
    QString t = QStringLiteral("Player  HPOS  SIZE    GRAF       COLPM\n");
    for (int i = 0; i < 4; ++i) {
        appendf(t, "P%d      $%02X   %-7s ", i, g.hposp[i], sizes[g.sizep[i] & 3]);
        grafBits(t, g.grafp[i], 8);
        appendf(t, "   $%02X\n", g.colpm[i]);
    }
    t += QStringLiteral("\nMissile HPOS  SIZE    GRAF\n");
    for (int i = 0; i < 4; ++i) {
        appendf(t, "M%d      $%02X   %-7s ", i, g.hposm[i], sizes[(g.sizem >> (i * 2)) & 3]);
        grafBits(t, (g.grafm >> (i * 2)) & 3, 2);
        t += QLatin1Char('\n');
    }
    const char *prio;
    switch (g.prior & 0x0F) {
    case 1: prio = "players > playfield"; break;
    case 2: prio = "P0-1 > playfield > P2-3"; break;
    case 4: prio = "playfield > players"; break;
    case 8: prio = "PF0-1 > players > PF2-3"; break;
    case 0: prio = "none set (overlaps mix)"; break;
    default: prio = "several set (overlaps mix)"; break;
    }
    appendf(t,
        "\nCOLPF0-3 $%02X $%02X $%02X $%02X   COLBK $%02X\n"
        "PRIOR   $%02X   priority %s\n"
        "              fifth player %s, multicolour players %s\n"
        "              GTIA mode %s\n"
        "VDELAY  $%02X   GRACTL $%02X (missiles %s, players %s, trigger latch %s)\n"
        "CONSOL  $%02X  %s%s%s%s\n"
        "TRIG0-3 %d %d %d %d (0 = pressed)\n\n",
        g.colpf[0], g.colpf[1], g.colpf[2], g.colpf[3], g.colbk,
        g.prior, prio, onOff(g.prior & 0x10), onOff(g.prior & 0x20), modes[g.prior >> 6],
        g.vdelay, g.gractl, onOff(g.gractl & 1), onOff(g.gractl & 2), onOff(g.gractl & 4),
        g.consol, (g.consol & 1) ? "" : " START", (g.consol & 2) ? "" : " SELECT",
        (g.consol & 4) ? "" : " OPTION", (g.consol & 7) == 7 ? " no console key held" : " held",
        g.trig[0] & 1, g.trig[1] & 1, g.trig[2] & 1, g.trig[3] & 1);
    t += QStringLiteral("Collisions  PF0 PF1 PF2 PF3    P0 P1 P2 P3\n");
    for (int i = 0; i < 4; ++i) {
        const char n[3] = { 'M', (char)('0' + i), 0 };
        collisionRow(t, n, g.mpf[i], g.mpl[i], -1);
    }
    for (int i = 0; i < 4; ++i) {
        const char n[3] = { 'P', (char)('0' + i), 0 };
        collisionRow(t, n, g.ppf[i], g.ppl[i], i);
    }
    setText(m_gtia, t);

    /* the colour registers through the palette */
    const uint8_t vals[SWATCHES] = { g.colpm[0], g.colpm[1], g.colpm[2], g.colpm[3],
                                     g.colpf[0], g.colpf[1], g.colpf[2], g.colpf[3], g.colbk };
    for (int i = 0; i < SWATCHES; ++i) {
        const uint32_t rgb = a52debug_color(m_dbg, vals[i]) & 0xFFFFFF;
        m_swatchLabel[i]->setText(QString::asprintf("%s $%02X", names[i], vals[i]));
        if (rgb != m_swatchRgb[i]) {
            m_swatchRgb[i] = rgb;
            m_swatch[i]->setStyleSheet(QStringLiteral("background: #%1; border: 1px solid rgba(128, 128, 128, 204);")
                                           .arg(rgb, 6, 16, QLatin1Char('0')));
        }
        m_swatch[i]->setToolTip(QString::asprintf("%s $%02X: #%06X", names[i], vals[i], rgb));
    }
    m_palette->ok = a52debug_palette_image(m_dbg, m_palette->px.data()) != 0;
    m_palette->update();
}

/* The IRQ bits by name, the set ones (or, activeLow, the clear ones). */
void irqWords(QString &t, uint8_t v, bool activeLow)
{
    static const char *const names[8] = {
        "timer 1", "timer 2", "timer 4", "serial output done", "serial output needed",
        "serial input ready", "key", "BREAK key" };
    bool any = false;
    for (int b = 7; b >= 0; --b)
        if ((((v >> b) & 1) != 0) != activeLow) {
            appendf(t, "%s%s", any ? ", " : " ", names[b]);
            any = true;
        }
    if (!any) t += QStringLiteral(" none");
}

void pokeyText(QString &t, const a52debug_pokey *p, const char *title)
{
    static const char *const dist[8] = {
        "5-bit + 17-bit poly", "5-bit poly", "5-bit + 4-bit poly", "5-bit poly",
        "17-bit poly", "pure tone", "4-bit poly", "pure tone" };
    static const char *const audctl[8] = {
        "15 kHz clock", "high-pass 2 by 4", "high-pass 1 by 3", "4 joined to 3 (16-bit)",
        "2 joined to 1 (16-bit)", "3 at 1.79 MHz", "1 at 1.79 MHz", "9-bit poly" };
    appendf(t, "%s\n  Channel  AUDF  AUDC  sound                  volume\n", title);
    for (int ch = 0; ch < 4; ++ch) {
        const int c = p->audc[ch], d = c >> 5;
        std::string sound = dist[d];
        if ((p->audctl & 0x80) && (d == 0 || d == 4)) {
            /* AUDCTL bit 7 swaps the 17-bit poly for the 9-bit one */
            const size_t at = sound.find("17-bit");
            if (at != std::string::npos) sound.replace(at, 6, "9-bit");
        }
        appendf(t, "  %d        $%02X   $%02X   %-22s %2d%s\n", ch + 1, p->audf[ch], c, sound.c_str(), c & 15,
                (c & 0x10) ? " (volume only)" : "");
    }
    appendf(t, "  AUDCTL   $%02X  ", p->audctl);
    bool any = false;
    for (int b = 0; b < 8; ++b)
        if (p->audctl & (1 << b)) {
            appendf(t, "%s%s", any ? ", " : " ", audctl[b]);
            any = true;
        }
    t += any ? QStringLiteral("\n") : QStringLiteral(" 64 kHz clock, nothing joined\n");
}

/* The pots of the four controllers: X and Y each, 0 .. 228 across the stick's
 * travel (114 at the centre). */
void potWords(QString &t, const a52debug_pokey &p)
{
    for (int c = 0; c < 4; ++c)
        appendf(t, "  controller %d   X $%02X (%3d)   Y $%02X (%3d)\n", c + 1, p.pot[c * 2], p.pot[c * 2],
                p.pot[c * 2 + 1], p.pot[c * 2 + 1]);
}

void DebuggerWindow::refreshPokey()
{
    a52debug_pokey p;
    QString t;

    a52debug_pokey_get(m_dbg, 0, &p);
    pokeyText(t, &p, "POKEY ($E800)");
    appendf(t,
        "\nKBCODE  $%02X   keypad / key code\n"
        "SKCTL   $%02X   keyboard scan %s, debounce %s, fast pot scan %s, two-tone %s, "
        "serial mode %d%s\n"
        "SKSTAT  $%02X  %s%s%s\n"
        "IRQEN   $%02X   enabled:",
        p.kbcode, p.skctl, onOff(p.skctl & 2), onOff(p.skctl & 1), onOff(p.skctl & 4),
        onOff(p.skctl & 8), (p.skctl >> 4) & 7, (p.skctl & 0x80) ? ", forcing a break" : "",
        p.skstat, (p.skstat & 8) ? "" : " SHIFT held", (p.skstat & 4) ? "" : " a key held",
        (p.skstat & 0x0C) == 0x0C ? " no key held" : "", p.irqen);
    irqWords(t, p.irqen, false);
    appendf(t, "\nIRQST   $%02X   pending:", p.irqst);
    irqWords(t, p.irqst, true);
    appendf(t, "\nRANDOM  $%02X\n\nPots (the analog sticks)\n", p.random);
    potWords(t, p);
    setText(m_pokey, t);
}

void DebuggerWindow::refreshMap()
{
    a52debug_map m;
    a52debug_map_get(m_dbg, &m);
    QString t;
    appendf(t, "Console    %s, %s\n\n", m.bios, m.tv_pal ? "PAL timing" : "NTSC");
    t += QStringLiteral("What the CPU sees\n"
                        "  $0000-$3FFF  RAM (16K)\n"
                        "  $4000-$BFFF  the cartridge (32K window)\n"
                        "  $C000-$C0FF  GTIA\n"
                        "  $D400-$D5FF  ANTIC\n"
                        "  $E800-$EFFF  POKEY\n"
                        "  $F800-$FFFF  BIOS (2K)\n\n");
    appendf(t, "Cartridge  %s%s\n", m.cart_name, m.cart_powered ? "" : " (powered off)");
    appendf(t, "  board    %s", m.mapper_name ? m.mapper_name : "?");
    if (m.mapper == 2)
        appendf(t, ", banks %d and %d", m.bank[0], m.bank[1]);
    else if (m.mapper == 3)
        appendf(t, ", %d banks of 32K, bank %d", m.nbanks, m.bank[0]);
    appendf(t, "\n  live     %u bytes, CRC %08X\n", m.live_size, m.live_crc);
    appendf(t, "  FUJI claim %s, mailbox %s, %d image swap%s since power-on\n",
            m.claim ? "yes" : "no", m.mailbox ? "owns $B000-$B7FF" : "closed", m.swaps, m.swaps == 1 ? "" : "s");
    appendf(t, "  link     %s%s\n", m.link_up ? "up" : "down", m.link_off ? " (FujiNet is off)" : "");
    appendf(t, "  boot     state %d, %d%%, error %d%s\n", m.boot_state, m.boot_pct, m.boot_err,
            m.booted_game ? ", a game has been served" : "");
    appendf(t, "  swap     %s%s", m.armed ? "armed" : "not armed", m.staged ? ", image staged" : ", nothing staged");
    if (m.staged)
        appendf(t, " (%u bytes, CRC %08X, %s%s)", m.staged_size, m.staged_crc,
                m.staged_claim ? "FUJI app" : "plain game", m.staged_config ? ", CONFIG" : "");
    appendf(t, "\n  queue    %u hotspot reads waiting\n\n", m.queue_depth);
    appendf(t, "Mailbox registers\n  ACKSEQ $%02X   STATUS $%02X   ERROR $%02X   REPLY CMD $%02X   RXLEN %u\n\n",
            m.ackseq, m.status, m.last_error, m.reply_cmd, m.rxlen);
    t += QStringLiteral("Arena as the console reads it (no side effects)\n"
                        "  $B400 status page\n");
    for (int row = 0; row < 4; ++row) {
        appendf(t, "  %04X ", 0xB400 + row * 16);
        for (int i = 0; i < 16; ++i) appendf(t, " %02X", m.arena[0x400 + row * 16 + i]);
        t += QLatin1Char('\n');
    }
    t += QStringLiteral("  $B000 reply window\n");
    for (int row = 0; row < 4; ++row) {
        appendf(t, "  %04X ", 0xB000 + row * 16);
        for (int i = 0; i < 16; ++i) appendf(t, " %02X", m.arena[row * 16 + i]);
        t += QLatin1Char('\n');
    }
    setText(m_map, cleanText(t.toUtf8().constData()));
}

const char *bpTypeName(int type)
{
    if (type & A52DEBUG_BP_EXEC) return "Execute";
    if ((type & (A52DEBUG_BP_READ | A52DEBUG_BP_WRITE)) == (A52DEBUG_BP_READ | A52DEBUG_BP_WRITE))
        return "Read/Write";
    return (type & A52DEBUG_BP_READ) ? "Read" : "Write";
}

void DebuggerWindow::refreshBps()
{
    static a52debug_breakpoint bps[BP_MAX];
    int max = 0;
    const int n = a52debug_breakpoint_list(m_dbg, bps, BP_MAX);
    const int used = a52debug_breakpoint_budget(m_dbg, &max);
    m_bpBudget->setText(QStringLiteral("atari800's condition table: %1 of %2 entries used").arg(used).arg(max));

    /* rebuilt only when it changed (each step refreshes) */
    QString sig;
    for (int i = 0; i < n; ++i)
        appendf(sig, "%d %d %d %d %d %s\n", bps[i].id, bps[i].type, bps[i].start, bps[i].end, bps[i].enabled,
                bps[i].condition);
    if (sig == m_bpSig && m_bpTable->rowCount() == n) return;
    m_bpSig = sig;

    QFont mono = m_bpTable->font();
    mono.setFamily(QStringLiteral("monospace"));
    mono.setStyleHint(QFont::TypeWriter);
    m_bpFilling = true;
    m_bpTable->setRowCount(n);
    for (int i = 0; i < n; ++i) {
        auto *on = new QTableWidgetItem;
        on->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
        on->setCheckState(bps[i].enabled ? Qt::Checked : Qt::Unchecked);
        on->setData(Qt::UserRole, bps[i].id);
        on->setToolTip(QStringLiteral("Enabled"));
        m_bpTable->setItem(i, 0, on);
        m_bpTable->setItem(i, 1, new QTableWidgetItem(QStringLiteral("#%1").arg(bps[i].id)));
        m_bpTable->setItem(i, 2, new QTableWidgetItem(QString::fromUtf8(bpTypeName(bps[i].type))));
        m_bpTable->setItem(i, 3, new QTableWidgetItem(bps[i].end != bps[i].start
            ? QString::asprintf("$%04X-$%04X", bps[i].start, bps[i].end) : QString::asprintf("$%04X", bps[i].start)));
        m_bpTable->setItem(i, 4, new QTableWidgetItem(bps[i].condition[0]
            ? QStringLiteral("if ") + QString::fromUtf8(bps[i].condition) : QString()));
        /* the rows in monospace, the header as everywhere else */
        for (int col = 1; col < 5; ++col) m_bpTable->item(i, col)->setFont(mono);
        auto *rm = new QToolButton;
        rm->setIcon(QIcon::fromTheme(QStringLiteral("edit-delete"), QIcon::fromTheme(QStringLiteral("user-trash"))));
        if (rm->icon().isNull()) rm->setText(QStringLiteral("Remove"));
        rm->setAutoRaise(true);
        rm->setToolTip(QStringLiteral("Remove"));
        const int id = bps[i].id;
        connect(rm, &QToolButton::clicked, this, [this, id] {
            a52debug_breakpoint_remove(m_dbg, id);
            syncLater();
        });
        m_bpTable->setCellWidget(i, 5, rm);
    }
    m_bpFilling = false;
    m_bpEmpty->setVisible(n == 0);
}

void DebuggerWindow::refreshFujinet(bool force)
{
    static a52_fuji_packet p[FUJI_MAX];
    static char state[2048];
    const int n = a52debug_fujinet_trace(m_dbg, p, FUJI_MAX);
    if (a52session_fujinet_external(m_session))
        m_fujiNote->setText(QStringLiteral("External FujiNet on BoIP %1, %2.")
                                .arg(QString::fromUtf8(a52session_fujinet_endpoint(m_session)),
                                     a52session_fujinet_link(m_session) ? QStringLiteral("connected")
                                                                         : QStringLiteral("not connected")));
    else if (!a52session_fujinet_running(m_session))
        m_fujiNote->setText(QStringLiteral("FujiNet is off (Preferences ▸ FujiNet)."));
    else
        m_fujiNote->setText(a52session_fujinet_link(m_session)
                                ? QStringLiteral("FujiNet is on BoIP, connected.")
                                : QStringLiteral("FujiNet is on BoIP, not connected."));
    state[0] = '\0';
    a52debug_fujinet_state(m_dbg, state, sizeof state);
    setText(m_fujiState, cleanText(state));

    /* a log: set again only when it moved */
    if (!force && n == m_fujiCount && (n == 0 || p[n - 1].serial == m_fujiLast)) return;
    m_fujiCount = n;
    m_fujiLast = n > 0 ? p[n - 1].serial : 0;
    QString t;
    for (int i = 0; i < n; ++i) {
        appendf(t, "%s%s\n     ", i ? "\n" : "", p[i].text);
        for (int k = 0; k < p[i].nbytes && k < (int)sizeof p[i].data; k++) appendf(t, "%02X ", p[i].data[k]);
        if (p[i].nbytes > (int)sizeof p[i].data) t += QStringLiteral("…");
    }
    if (n == 0) t = QStringLiteral("(no FujiBus packets since the trace was cleared)");
    setText(m_fujiText, cleanText(t.toUtf8().constData()), force || atEnd(m_fujiText));
}

void DebuggerWindow::refreshPage(int page)
{
    if (!a52session_is_running(m_session)) return;
    switch (page) {
    case TAB_CPU: refreshCpu(); refreshMem(); refreshHistory(); break;
    case TAB_DISASM: refreshDisasm(); break;
    case TAB_ANTIC: refreshAntic(); break;
    case TAB_GTIA: refreshGtia(); break;
    case TAB_POKEY: refreshPokey(); break;
    case TAB_MAP: refreshMap(); break;
    case TAB_BREAKS: refreshBps(); break;
    case TAB_FUJINET: refreshFujinet(false); break;
    default: break;
    }
}

void DebuggerWindow::refreshAll()
{
    refreshStatus();
    refreshPage(m_tabs->currentIndex());
}

/* After anything that may have moved the engine: the console takes what
 * atari800 printed at a stop, then the views follow. The drain moves the
 * generation itself, so it is read after. */
void DebuggerWindow::sync()
{
    m_wasStopped = a52debug_is_stopped(m_dbg) != 0;
    if (m_wasStopped) consoleDrain();
    m_seenGen = a52debug_generation(m_dbg);
    m_runningTicks = 0;
    refreshAll();
}

/* From inside a breakpoint row's own signal: the table rebuilds after. */
void DebuggerWindow::syncLater()
{
    QTimer::singleShot(0, this, [this] { sync(); });
}

/* A step takes microseconds: a moment's wait for the stop keeps the window
 * from flashing "Running" in between. */
void DebuggerWindow::settleAndSync()
{
    for (int i = 0; i < 40 && !a52debug_is_stopped(m_dbg); ++i) QThread::msleep(1);
    sync();
}

void DebuggerWindow::attachAndRefresh()
{
    a52debug_attach(m_dbg);    /* stops the machine */
    a52debug_stop(m_dbg);      /* ... and so does showing it when it was up and running */
    m_attachedRunning = a52session_is_running(m_session) != 0;
    settleAndSync();
}

void DebuggerWindow::tick()
{
    if (!isVisible()) return;
    if (!a52session_is_running(m_session)) {
        if (m_attachedRunning) {
            m_attachedRunning = false;
            refreshStatus();
        }
        return;
    }
    /* Opened before the session started, or showing across a restart: the
     * engine attaches (stopping the machine) as it comes up. */
    if (!m_attachedRunning) {
        a52debug_detach(m_dbg);
        attachAndRefresh();
        return;
    }
    const unsigned gen = a52debug_generation(m_dbg);
    const bool stopped = a52debug_is_stopped(m_dbg) != 0;
    if (gen != m_seenGen || stopped != m_wasStopped) {
        sync();
    } else if (!stopped && ++m_runningTicks >= 5) {
        /* live values twice a second while the machine runs */
        const int page = m_tabs->currentIndex();
        m_runningTicks = 0;
        refreshStatus();
        if (page == TAB_CPU) refreshCpu();
        if (page == TAB_FUJINET) refreshFujinet(false);
    }
}

/* ---- the console ---------------------------------------------------------- */

void DebuggerWindow::consoleAppend(const QString &text)
{
    QTextCursor cur(m_conOut->document());
    cur.movePosition(QTextCursor::End);
    cur.insertText(text);
    m_conOut->verticalScrollBar()->setValue(m_conOut->verticalScrollBar()->maximum());
}

/* Command output, ended with a newline. */
void DebuggerWindow::consoleOutput(const char *text)
{
    const QString t = cleanText(text);
    if (t.isEmpty()) return;
    consoleAppend(t.endsWith(QLatin1Char('\n')) ? t : t + QLatin1Char('\n'));
}

/* Messages from Load Symbols and Save land in the console and, since it may
 * not be the tab showing, the status bar. */
void DebuggerWindow::say(const QString &msg)
{
    consoleOutput(msg.toUtf8().constData());
    message(msg);
}

void DebuggerWindow::message(const QString &msg)
{
    statusBar()->showMessage(msg, 6000);
}

void DebuggerWindow::setPrompt(bool asmMode, const QString &prompt)
{
    m_conAsm = asmMode;
    m_conPromptText = asmMode ? prompt.left(32) : QStringLiteral("> ");
    m_conPrompt->setText(m_conPromptText);
    m_conIn->setPlaceholderText(asmMode
        ? QStringLiteral("Assembler: an instruction (LDA #$42), or an empty line to finish")
        : QStringLiteral("Monitor command (HELP, D, M, F, A, BLINE, DLIST, LABELS, CONT ...) "
                         "— Tab completes, Up/Down recall"));
}

/* The banner and registers atari800 printed at a stop: an empty line,
 * which its monitor ignores, hands them over. Not while the assembler is
 * waiting, where an empty line would end it. */
void DebuggerWindow::consoleDrain()
{
    static char out[65536];
    if (m_conAsm || !a52session_is_running(m_session)) return;
    a52debug_command(m_dbg, "", out, sizeof out);
    consoleOutput(out);
}

void DebuggerWindow::runConsole()
{
    static char out[65536];
    const QString cmd = m_conIn->text().trimmed();
    /* an empty line only means something to the assembler (its end) */
    if (!m_conAsm && cmd.isEmpty()) return;
    if (!cmd.isEmpty() && (m_history.isEmpty() || m_history.last() != cmd)) m_history << cmd;
    m_historyPos = (int)m_history.size();
    consoleAppend(m_conPromptText + cmd + QLatin1Char('\n'));
    m_conIn->clear();

    const int rc = a52debug_command(m_dbg, cmd.toUtf8().constData(), out, sizeof out);
    if (rc == A52DBG_CMD_NEEDS_INPUT) {
        /* the output ends with the assembler's own prompt ("0642: ") */
        char *nl = strrchr(out, '\n');
        char *prompt = nl ? nl + 1 : out;
        const QString saved = QString::fromUtf8(prompt);
        *prompt = '\0';
        consoleOutput(out);
        setPrompt(true, saved);
    } else {
        consoleOutput(out);
        setPrompt(false, QString());
    }
    if (rc == A52DBG_CMD_RESUMED) settleAndSync();
    else sync();
}

void DebuggerWindow::showHistory()
{
    m_conIn->setText(m_historyPos < (int)m_history.size() ? m_history.at(m_historyPos) : QString());
    m_conIn->end(false);
}

/* Tab completes the command word against atari800's commands (the longest
 * common start of several, which are listed). */
void DebuggerWindow::complete()
{
    const QString text = m_conIn->text();
    if (m_conAsm || text.contains(QLatin1Char(' '))) {
        QApplication::beep();
        return;
    }
    char comps[2048];
    a52debug_completions(m_dbg, text.toUtf8().constData(), comps, sizeof comps);
    const QStringList words = QString::fromUtf8(comps).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (words.isEmpty()) {
        QApplication::beep();
        return;
    }
    if (words.size() == 1) {
        m_conIn->setText(words[0] + QLatin1Char(' '));
        return;
    }
    qsizetype len = words[0].size();
    for (const QString &w : words) {
        qsizetype k = 0;
        while (k < len && k < w.size() && w[k] == words[0][k]) ++k;
        len = k;
    }
    if (len > text.size()) m_conIn->setText(words[0].left(len));
    consoleOutput(words.join(QStringLiteral("  ")).toUtf8().constData());
}

/* ---- the rest of the handlers ----------------------------------------------- */

void DebuggerWindow::toggleRun()
{
    if (a52debug_is_stopped(m_dbg)) a52debug_resume(m_dbg);
    else a52debug_stop(m_dbg);
    sync();
}

void DebuggerWindow::stepAnd(void (*fn)(a52debug *))
{
    fn(m_dbg);
    settleAndSync();
}

void DebuggerWindow::runToCursor()
{
    if (m_selected < 0) {
        message(QStringLiteral("Select a line in the Disassembly to run to"));
        return;
    }
    a52debug_run_to(m_dbg, (uint16_t)m_selected);
    settleAndSync();
}

void DebuggerWindow::toggleBreakpoint(uint16_t addr)
{
    if (a52debug_breakpoint_toggle(m_dbg, addr) < 0)
        message(QStringLiteral("atari800's condition table is full: remove a breakpoint first"));
    sync();
}

/* A label, or a number as parseNum reads it. */
bool DebuggerWindow::addressOf(const QString &text, long *out)
{
    const QString t = text.trimmed();
    if (t.isEmpty()) return false;
    const int a = a52debug_label_address(m_dbg, t.toUtf8().constData());
    if (a >= 0) {
        *out = a;
        return true;
    }
    return parseNum(t, out);
}

void DebuggerWindow::addBreakpoint()
{
    static const int types[4] = { A52DEBUG_BP_EXEC, A52DEBUG_BP_READ, A52DEBUG_BP_WRITE,
                                  A52DEBUG_BP_READ | A52DEBUG_BP_WRITE };
    char msg[200] = "";
    long a, b;
    if (!addressOf(m_bpStart->text(), &a)) {
        m_bpMsg->setText(QStringLiteral("Not an address or a label"));
        return;
    }
    b = a;
    if (!m_bpEnd->text().trimmed().isEmpty() && !addressOf(m_bpEnd->text(), &b)) {
        m_bpMsg->setText(QStringLiteral("The end is not an address or a label"));
        return;
    }
    const int sel = qBound(0, m_bpType->currentIndex(), 3);
    const QByteArray cond = m_bpCond->text().trimmed().toUtf8();
    if (a52debug_breakpoint_add(m_dbg, types[sel], (uint16_t)a, (uint16_t)b, cond.constData(), msg, sizeof msg) < 0) {
        m_bpMsg->setText(msg[0] ? QString::fromUtf8(msg) : QStringLiteral("atari800 did not take it"));
        return;
    }
    m_bpMsg->clear();
    m_bpStart->clear();
    m_bpEnd->clear();
    m_bpCond->clear();
    sync();
}

/* A scanline break is atari800's own (BLINE): the next Run stops there. */
void DebuggerWindow::breakAtLine()
{
    static char out[1024];
    bool ok = false;
    const int n = m_bpLine->text().trimmed().toInt(&ok, 10);
    if (!ok || n < 0 || n > 311) {
        m_bpMsg->setText(QStringLiteral("A scanline is 0-311 (decimal)"));
        return;
    }
    /* the assembler would take the command as its next line */
    if (m_conAsm) {
        m_bpMsg->setText(QStringLiteral("Finish the assembler in the Console first (an empty line ends it)"));
        return;
    }
    const QByteArray cmd = QStringLiteral("BLINE %1").arg(n).toUtf8();
    a52debug_command(m_dbg, cmd.constData(), out, sizeof out);
    if (char *nl = strchr(out, '\n')) *nl = '\0';
    m_bpMsg->setText(out[0] ? cleanText(out) : QString::fromUtf8(cmd));
    sync();
}

/* Go to: the dump from just above an address (or a label), its row marked
 * and scrolled to. */
void DebuggerWindow::memGoto()
{
    long a;
    if (!addressOf(m_memGoto->text(), &a)) {
        m_memMark = -1;
        refreshMem();
        message(QStringLiteral("Not an address or a label"));
        return;
    }
    m_memMark = (int)(a & 0xFFFF);
    m_memTop = (m_memMark - 0x100) & 0xFFF0;
    refreshMem();
    const int line = 1 + ((m_memMark - m_memTop) & 0xFFFF) / 16;
    const int shown = m_mem->viewport()->height() / qMax(1, m_mem->fontMetrics().lineSpacing());
    m_mem->verticalScrollBar()->setValue(qMax(0, line - shown * 3 / 10));
}

void DebuggerWindow::memWrite()
{
    long a, v;
    if (!addressOf(m_memAddr->text(), &a)) {
        message(QStringLiteral("Not an address or a label"));
        return;
    }
    if (!parseNum(m_memVal->text(), &v) || v < 0 || v > 0xFF) {
        message(QStringLiteral("The value is a byte ($00-$FF)"));
        return;
    }
    a52debug_write(m_dbg, (uint16_t)a, (uint8_t)v);
    sync();
}

void DebuggerWindow::jumpTo()
{
    long a;
    if (!addressOf(m_jump->text(), &a)) {
        message(QStringLiteral("Not an address or a label"));
        return;
    }
    m_followPc->setChecked(false);
    m_disasmTop = (int)(a & 0xFFFF);
    m_selected = m_disasmTop;
    refreshDisasm();
}

void DebuggerWindow::loadSymbols()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Load Symbols (ld65 -Ln, VICE .lbl, MADS .lab, atari800 labels)"), QString(),
        QStringLiteral("Label files (*.lbl *.lab *.sym *.txt);;All files (*)"));
    if (path.isEmpty()) return;
    char msg[512];
    if (a52debug_load_symbols(m_dbg, path.toLocal8Bit().constData(), msg, sizeof msg) >= 0)
        say(QStringLiteral("Loaded %1 from %2").arg(QString::fromUtf8(msg), QFileInfo(path).fileName()));
    else
        say(QStringLiteral("No symbols loaded: %1").arg(QString::fromUtf8(msg)));
    sync();
}

void DebuggerWindow::saveFile(const char *kind, const char *title, const char *file)
{
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save %1").arg(QString::fromUtf8(title)),
                                                      QString::fromUtf8(file));
    if (path.isEmpty()) return;
    char msg[512];
    if (a52debug_save(m_dbg, kind, path.toLocal8Bit().constData(), msg, sizeof msg) == 0)
        say(QStringLiteral("%1: %2").arg(QFileInfo(path).fileName(), QString::fromUtf8(msg)));
    else
        say(QString::fromUtf8(msg));
}

bool DebuggerWindow::eventFilter(QObject *obj, QEvent *e)
{
    /* Up and Down walk the history; Tab completes */
    if (obj == m_conIn && e->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(e);
        if (ke->key() == Qt::Key_Up) {
            if (m_historyPos > 0) {
                --m_historyPos;
                showHistory();
            }
            return true;
        }
        if (ke->key() == Qt::Key_Down) {
            if (m_historyPos < (int)m_history.size()) {
                ++m_historyPos;
                showHistory();
            }
            return true;
        }
        if (ke->key() == Qt::Key_Tab || ke->key() == Qt::Key_Backtab) {
            complete();
            return true;
        }
    }
    if (m_disasm && obj == m_disasm->viewport()) {
        /* A click selects a line (Run to Cursor); a click on its left edge,
         * or a double-click, toggles its breakpoint. */
        if (e->type() == QEvent::MouseButtonPress || e->type() == QEvent::MouseButtonDblClick) {
            auto *me = static_cast<QMouseEvent *>(e);
            if (me->button() != Qt::LeftButton) return true;
            const QTextCursor cur = m_disasm->cursorForPosition(me->position().toPoint());
            const int line = cur.blockNumber();
            if (line < 0 || line >= (int)m_lineAddr.size()) return true;
            const bool gutter = cur.positionInBlock() <= 2;
            const bool dbl = e->type() == QEvent::MouseButtonDblClick;
            m_selected = m_lineAddr[line];
            if (gutter ? !dbl : dbl) toggleBreakpoint(m_lineAddr[line]);
            else refreshDisasm();
            return true;
        }
        if (e->type() == QEvent::MouseButtonRelease || e->type() == QEvent::MouseMove) return true;
        if (e->type() == QEvent::Wheel) {
            m_wheel += static_cast<QWheelEvent *>(e)->angleDelta().y();
            const int rows = -m_wheel / 40;     /* a notch is three rows */
            if (rows != 0 && a52session_is_running(m_session)) {
                m_wheel += rows * 40;
                m_followPc->setChecked(false);
                m_disasmTop = a52debug_row_address(m_dbg, (uint16_t)m_disasmTop, rows);
                refreshDisasm();
            }
            return true;
        }
        if (e->type() == QEvent::Resize && isVisible() && a52session_is_running(m_session)) refreshDisasm();
    }
    return QMainWindow::eventFilter(obj, e);
}

/* The family's keys: F5 run/stop, F7 step, F8 step over, Shift+F8 step out,
 * F9 frame, F12 close. */
bool isDebuggerKey(const QKeyEvent *e)
{
    const Qt::KeyboardModifiers mods = e->modifiers() & ~Qt::KeypadModifier;
    switch (e->key()) {
    case Qt::Key_F5: case Qt::Key_F7: case Qt::Key_F9: case Qt::Key_F12: return mods == Qt::NoModifier;
    case Qt::Key_F8: return mods == Qt::NoModifier || mods == Qt::ShiftModifier;
    default: return false;
    }
}

/* The main window's F9 (Controllers) and F12 are application-wide shortcuts:
 * here these keys are the debugger's, taken as plain key presses ahead of
 * them (Shift+F5, Power Cycle, stays the main window's). */
bool DebuggerWindow::event(QEvent *e)
{
    if (e->type() == QEvent::ShortcutOverride && isDebuggerKey(static_cast<QKeyEvent *>(e))) {
        e->accept();
        return true;
    }
    return QMainWindow::event(e);
}

void DebuggerWindow::keyPressEvent(QKeyEvent *e)
{
    if (!isDebuggerKey(e)) {
        QMainWindow::keyPressEvent(e);
        return;
    }
    /* held, the steps repeat; the toggles do not */
    if (e->isAutoRepeat() && (e->key() == Qt::Key_F5 || e->key() == Qt::Key_F12)) return;
    switch (e->key()) {
    case Qt::Key_F5: toggleRun(); break;
    case Qt::Key_F7: stepAnd(a52debug_step); break;
    case Qt::Key_F8:
        stepAnd((e->modifiers() & Qt::ShiftModifier) ? a52debug_step_out : a52debug_step_over);
        break;
    case Qt::Key_F9: stepAnd(a52debug_frame); break;
    default: hide(); break;
    }
}

void DebuggerWindow::showEvent(QShowEvent *e)
{
    m_timer.start(100);
    QMainWindow::showEvent(e);
}

/* Hidden by F12, the close button or the main window: the engine goes
 * away and the machine runs on. The close button is a plain close, which
 * only hides the window (it is kept, and comes back as it was). A
 * minimised window's spontaneous hide is not a detach. */
void DebuggerWindow::hideEvent(QHideEvent *e)
{
    m_timer.stop();
    if (!e->spontaneous()) {
        a52debug_detach(m_dbg);      /* lets it run; a waiting assembler gives up */
        m_attachedRunning = false;
        setPrompt(false, QString());
    }
    QMainWindow::hideEvent(e);
}

} // namespace

void a52DebuggerShow(QWidget *parent, a52session *session)
{
    if (!s_win) s_win = new DebuggerWindow(session, parent);
    s_win->show();
    s_win->raise();
    s_win->activateWindow();
    s_win->attachAndRefresh();
}

void a52DebuggerToggle(QWidget *parent, a52session *session)
{
    if (s_win && s_win->isVisible()) s_win->hide();
    else a52DebuggerShow(parent, session);
}
