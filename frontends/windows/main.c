/*
 * FujiNet Go Atari 5200 -- the Windows (Win32) frontend.
 *
 * No toolkit: a plain window, a menu bar and a status bar, with the picture a
 * child window drawn by display.c (Direct3D 11, GDI as the fallback). That
 * keeps the artifact a folder you copy rather than a runtime hunt.
 *
 * Three Windows-specific things are load bearing:
 *
 *   The display's render thread is this platform's frame clock: it feeds each
 *   vblank to the core (display.c). There is no GdkFrameClock here, and a
 *   plain timer would beat against the panel.
 *
 *   WM_ACTIVATE releases every held key. Alt-tabbing away mid-game and coming
 *   back to a stick still pushed is the classic symptom of not doing this, and
 *   Windows is where it happens most, because the WM eats the key-up.
 *
 *   The 5200 has no Alt key, so a system key (WM_SYSKEYDOWN) is passed to the
 *   session and swallowed only when it is bound: Alt+F4, the menu mnemonics
 *   and Alt+Esc stay Windows'.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#include "a52session.h"
#include "controllers/controllers_window.h"
#include "debugger/dbg_window.h"
#include "display.h"
#include "fujilog.h"
#include "help/help_window.h"
#include "import.h"
#include "key_forward.h"
#include "prefs.h"
#include "resource.h"
#include "win_common.h"

#define WIN_CLASS "FujiNetGoA52"
#define APP_TITLE "FujiNet Go Atari 5200"
#define APP_TITLE_W L"FujiNet Go Atari 5200"
#define INSTANCE_MUTEX "Local\\FujiNetGoA52.Instance"

/* How long a notice (a gamepad hot-plug, a CIM) stays in the status bar. */
#define NOTICE_MS 4000

/* The status bar's first part: the FujiNet link dot. */
#define DOT_PART_W 22

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

static a52session *g_session;
static HWND g_hwnd, g_display, g_statusbar;
static int g_aspect = 1, g_smooth = 0, g_fullscreen = 0;    /* the TV's 4:3 on */
static WINDOWPLACEMENT g_placement;
static int g_sysact_down[A52_SYSACT_COUNT];
static unsigned g_pad_generation, g_fuji_generation;
static char g_notice[160];
static DWORD g_notice_until;
static HBRUSH g_accent_brush;
static char g_shot_path[MAX_PATH];
static LONG g_shot_frames = 300, g_quit_frames;
/* the display drag that drives controller 1's analog stick */
static int g_drag, g_drag_x, g_drag_y;

static void update_status(void);
static void sync_menu_checks(void);
static void layout_children(void);

/* ---- small helpers -------------------------------------------------------- */

static int S(int v) { return a52_scale(v, a52_window_dpi(g_hwnd)); }

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '\\');
    const char *t = strrchr(p, '/');
    if (t && (!s || t > s)) s = t;
    return s ? s + 1 : p;
}

static int env_on(const char *name)
{
    const char *env = getenv(name);
    return env && *env && *env != '0';
}

static void notice(const char *text)
{
    snprintf(g_notice, sizeof g_notice, "%s", text);
    g_notice_until = GetTickCount() + NOTICE_MS;
    update_status();
}

/* ---- A52_SCREENSHOT: the picture as a PNG (zlib is the core's) -------------- */

static void png_chunk(FILE *f, const char *type, const unsigned char *data, unsigned len)
{
    unsigned char hdr[8];
    uLong crc = crc32(0, (const Bytef *)type, 4);
    hdr[0] = (unsigned char)(len >> 24); hdr[1] = (unsigned char)(len >> 16);
    hdr[2] = (unsigned char)(len >> 8); hdr[3] = (unsigned char)len;
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (len) {
        fwrite(data, 1, len, f);
        crc = crc32(crc, data, len);
    }
    hdr[0] = (unsigned char)(crc >> 24); hdr[1] = (unsigned char)(crc >> 16);
    hdr[2] = (unsigned char)(crc >> 8); hdr[3] = (unsigned char)crc;
    fwrite(hdr, 1, 4, f);
}

/* The newest frame the core has published, as an RGB PNG. */
static int save_png(const char *path)
{
    static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    static uint32_t fb[A52SESSION_FB_MAX_WIDTH * A52SESSION_FB_MAX_HEIGHT];
    unsigned char ihdr[13];
    unsigned char *raw, *z;
    uLongf zlen;
    uint64_t serial = 0;
    int w = 0, h = 0, par = 1000, x, y, ok = 0;
    FILE *f;

    if (!a52session_copy_frame(g_session, fb, &w, &h, &par, &serial) || w <= 0 || h <= 0)
        return 0;
    raw = malloc((size_t)(w * 3 + 1) * (size_t)h);
    if (!raw) return 0;
    for (y = 0; y < h; y++) {
        unsigned char *row = raw + (size_t)y * (size_t)(w * 3 + 1);
        *row++ = 0;   /* no filter */
        for (x = 0; x < w; x++) {
            const uint32_t p = fb[(size_t)y * (size_t)w + (size_t)x];
            *row++ = (unsigned char)(p >> 16);
            *row++ = (unsigned char)(p >> 8);
            *row++ = (unsigned char)p;
        }
    }
    zlen = compressBound((uLong)((w * 3 + 1) * h));
    z = malloc(zlen);
    if (z && compress(z, &zlen, raw, (uLong)((w * 3 + 1) * h)) == Z_OK && (f = fopen(path, "wb"))) {
        ihdr[0] = (unsigned char)(w >> 24); ihdr[1] = (unsigned char)(w >> 16);
        ihdr[2] = (unsigned char)(w >> 8); ihdr[3] = (unsigned char)w;
        ihdr[4] = (unsigned char)(h >> 24); ihdr[5] = (unsigned char)(h >> 16);
        ihdr[6] = (unsigned char)(h >> 8); ihdr[7] = (unsigned char)h;
        ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = ihdr[11] = ihdr[12] = 0;   /* 8-bit RGB */
        fwrite(sig, 1, 8, f);
        png_chunk(f, "IHDR", ihdr, 13);
        png_chunk(f, "IDAT", z, (unsigned)zlen);
        png_chunk(f, "IEND", NULL, 0);
        ok = fclose(f) == 0;
    }
    free(z);
    free(raw);
    return ok;
}

/* ---- layout: picture, status bar --------------------------------------- */

static int statusbar_height(void)
{
    RECT r;
    if (!g_statusbar || !IsWindowVisible(g_statusbar)) return 0;
    GetWindowRect(g_statusbar, &r);
    return r.bottom - r.top;
}

/* The picture fills the client area above the status bar. */
static void layout_children(void)
{
    RECT rc;
    int bottom;
    if (!g_display) return;
    GetClientRect(g_hwnd, &rc);
    bottom = rc.bottom - statusbar_height();
    if (bottom < 0) bottom = 0;
    MoveWindow(g_display, 0, 0, rc.right, bottom, TRUE);
}

/* ---- the console ----------------------------------------------------------------- */

static void power_cycle(void)
{
    a52session_power_cycle(g_session);
    a52_controllers_window_refresh();
    update_status();
}

static void run_sysaction(int sa)
{
    switch (sa) {
    case A52_SYSACT_POWER:
        power_cycle();
        break;
    case A52_SYSACT_PAUSE:
        /* Showing the debugger attaches it, and attaching stops the machine. */
        a52_debugger_show(g_hwnd, g_session);
        break;
    case A52_SYSACT_WARP:
        a52session_set_warp(g_session, !a52session_warp(g_session));
        sync_menu_checks();
        break;
    default: break;
    }
}

/* Open anything: a cartridge is served by the FujiNet cartridge and the
 * console power-cycled onto it, a BIOS (file, ZIP or folder) is imported,
 * anything else is copied to FujiNet's SD folder for CONFIG to mount. */
static void load_media(const char *path)
{
    char msg[1024];
    int kind;
    msg[0] = '\0';
    kind = a52session_open_or_import(g_session, path, msg, sizeof msg);
    if (kind < 0) {
        a52_message(g_hwnd, msg[0] ? msg : "Could not open that file.", "Could not open",
                    MB_ICONWARNING | MB_OK);
    } else if (kind == A52_MEDIA_BIOS) {
        a52_show_import_message(g_hwnd, 1, msg);
        a52_controllers_window_refresh();
    } else if (kind == A52_MEDIA_UNKNOWN) {
        a52_message(g_hwnd, msg[0] ? msg : "Copied to FujiNet's SD folder. Mount it from the CONFIG client.",
                    "Imported", MB_ICONINFORMATION | MB_OK);
    } else if (msg[0]) {
        notice(msg);
    }
    update_status();
}

/* ---- status ------------------------------------------------------------------------- */

static void set_text_utf8(HWND h, const char *text, int statusbar)
{
    wchar_t w[512];
    MultiByteToWideChar(CP_UTF8, 0, text, -1, w, 512);
    if (statusbar) SendMessageW(h, SB_SETTEXTW, 1, (LPARAM)w);   /* part 1: the text */
    else SetWindowTextW(h, w);
}

static void update_status(void)
{
    char title[512], st[160], line[400], info[160];
    const char *cart = a52session_cart_path(g_session);

    if (!a52session_is_running(g_session)) {
        snprintf(title, sizeof title, APP_TITLE " \xe2\x80\x94 stopped");
        snprintf(st, sizeof st, "stopped");
        info[0] = '\0';
    } else {
        a52session_cart_status(g_session, st, sizeof st);
        a52session_cart_info(g_session, info, sizeof info);
        if (cart && cart[0])
            snprintf(title, sizeof title, APP_TITLE " \xe2\x80\x94 %s", base_name(cart));
        else if (a52session_cart_booted_game(g_session))
            snprintf(title, sizeof title, APP_TITLE " \xe2\x80\x94 booted from FujiNet");
        else
            snprintf(title, sizeof title, APP_TITLE " \xe2\x80\x94 CONFIG");
    }
    set_text_utf8(g_hwnd, title, 0);

    if (!g_statusbar) return;
    /* Part 0 is the link dot, drawn in WM_DRAWITEM from this item data. */
    SendMessageA(g_statusbar, SB_SETTEXTA, SBT_OWNERDRAW | 0,
                 (LPARAM)(a52session_fujinet_link(g_session) == 1));
    if (g_notice[0] && GetTickCount() < g_notice_until) {
        snprintf(line, sizeof line, "%s", g_notice);
    } else {
        g_notice[0] = '\0';
        snprintf(line, sizeof line, "%s%sFujiNet: %s%s%s", info, info[0] ? " \xe2\x80\x94 " : "", st,
                 a52session_fujinet_external(g_session) ? " (external)"
                 : a52session_fujinet_running(g_session) ? "" : " (runtime not running)",
                 a52session_is_paused(g_session) ? " \xe2\x80\x94 paused" : "");
    }
    set_text_utf8(g_statusbar, line, 1);
}

/* The link dot: the accent while the cartridge's link to FujiNet is up, grey
 * otherwise. */
static void draw_status_dot(const DRAWITEMSTRUCT *di)
{
    const int up = di->itemData != 0;
    const int size = S(10);
    RECT r = di->rcItem;
    const int x = r.left + ((r.right - r.left) - size) / 2;
    const int y = r.top + ((r.bottom - r.top) - size) / 2;
    HGDIOBJ oldb = SelectObject(di->hDC, up ? (HGDIOBJ)g_accent_brush : GetStockObject(GRAY_BRUSH));
    HGDIOBJ oldp = SelectObject(di->hDC, GetStockObject(NULL_PEN));
    Ellipse(di->hDC, x, y, x + size + 1, y + size + 1);
    SelectObject(di->hDC, oldp);
    SelectObject(di->hDC, oldb);
}

/* Gamepad hot-plug and FujiNet restarts: the session bumps a generation on
 * every one and keeps the last event as text; show it for a few seconds. */
static void poll_events(void)
{
    const unsigned gen = a52session_gamepad_generation(g_session);
    const unsigned fgen = a52session_fujinet_generation(g_session);
    char text[160];
    if (gen != g_pad_generation) {
        g_pad_generation = gen;
        if (a52session_gamepad_last_event(g_session, text, sizeof text) > 0)
            notice(text);
        a52_controllers_window_refresh();
    }
    if (fgen != g_fuji_generation) {
        g_fuji_generation = fgen;
        if (a52session_fujinet_last_event(g_session, text, sizeof text) > 0)
            notice(text);
    }
    /* a CIM or other stop the user should hear about */
    if (a52session_take_notice(g_session, text, sizeof text))
        notice(text);
}

/* ---- menu ----------------------------------------------------------------------------- */

/* A menu item's label with the key it is bound to after the tab -- read from
 * the bindings each time the menu opens, so a remap in the Controllers window
 * shows here. The key is only a label: there is no accelerator table, so the
 * console's keys reach it through the bindings rather than being eaten by
 * the menu. */
static void bound_item(HMENU m, UINT id, const char *text, int target)
{
    char key[32], label[96];
    const a52_binding b = a52session_binding_get(g_session, target);
    key[0] = '\0';
    a52session_keysym_name(b.keysym, key, sizeof key);
    if (key[0]) snprintf(label, sizeof label, "%s\t%s", text, key);
    else snprintf(label, sizeof label, "%s", text);
    ModifyMenuA(m, id, MF_BYCOMMAND | MF_STRING, id, label);
}

static void sync_console_menu(void)
{
    HMENU m = GetMenu(g_hwnd);
    if (!m) return;
    bound_item(m, IDM_RESET, "&Reset", A52_TARGET_KEY(A52_KEY_RESET));
    CheckMenuItem(m, IDM_PAUSE, MF_BYCOMMAND | (a52session_is_paused(g_session) ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, IDM_WARP, MF_BYCOMMAND | (a52session_warp(g_session) ? MF_CHECKED : MF_UNCHECKED));
}

static void sync_menu_checks(void)
{
    HMENU m = GetMenu(g_hwnd);
    if (!m) return;
    CheckMenuItem(m, IDM_TV_ASPECT, MF_BYCOMMAND | (g_aspect ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, IDM_SMOOTH, MF_BYCOMMAND | (g_smooth ? MF_CHECKED : MF_UNCHECKED));
    sync_console_menu();
}

static void build_menu(HWND hwnd)
{
    HMENU bar = CreateMenu();
    HMENU console = CreatePopupMenu();
    HMENU bios = CreatePopupMenu();
    HMENU view = CreatePopupMenu();
    HMENU fuji = CreatePopupMenu();
    HMENU help = CreatePopupMenu();

    AppendMenuA(bios, MF_STRING, IDM_BIOS_FILES, "&Files...");
    AppendMenuA(bios, MF_STRING, IDM_BIOS_ZIP, "&ZIP...");
    AppendMenuA(bios, MF_STRING, IDM_BIOS_FOLDER, "F&older...");

    AppendMenuA(console, MF_STRING, IDM_OPEN, "&Open...\tCtrl+Shift+O");
    AppendMenuA(console, MF_STRING, IDM_IMPORT_SD, "Import to &SD...");
    AppendMenuA(console, MF_POPUP, (UINT_PTR)bios, "Import &BIOS");
    AppendMenuA(console, MF_SEPARATOR, 0, NULL);
    AppendMenuA(console, MF_STRING, IDM_POWER, "Power &Cycle (Reboot to CONFIG)\tShift+F5");
    AppendMenuA(console, MF_STRING, IDM_RESET, "&Reset");
    AppendMenuA(console, MF_STRING, IDM_PAUSE, "&Pause");
    AppendMenuA(console, MF_STRING, IDM_WARP, "&Warp\tCtrl+Shift+W");
    AppendMenuA(console, MF_SEPARATOR, 0, NULL);
    AppendMenuA(console, MF_STRING, IDM_SETTINGS, "Pre&ferences...\tCtrl+,");
    AppendMenuA(console, MF_SEPARATOR, 0, NULL);
    AppendMenuA(console, MF_STRING, IDM_EXIT, "E&xit\tCtrl+Shift+Q");

    AppendMenuA(view, MF_STRING, IDM_CONTROLLERS, "&Controllers\tF9");
    AppendMenuA(view, MF_STRING, IDM_DEBUGGER, "&Debugger\tF12");
    AppendMenuA(view, MF_SEPARATOR, 0, NULL);
    AppendMenuA(view, MF_STRING, IDM_TV_ASPECT, "&TV Aspect");
    AppendMenuA(view, MF_STRING, IDM_SMOOTH, "&Smooth Scaling");
    AppendMenuA(view, MF_STRING, IDM_FULLSCREEN, "&Fullscreen\tF11");

    AppendMenuA(fuji, MF_STRING, IDM_FUJINET_CONFIG, "&Web UI");
    AppendMenuA(fuji, MF_STRING, IDM_FUJINET_LOG, "Console &Log");
    AppendMenuA(fuji, MF_STRING, IDM_FUJINET_RESTART, "&Restart FujiNet");

    AppendMenuA(help, MF_STRING, IDM_HELP_TOPICS, APP_TITLE " &Help\tCtrl+Shift+H");
    AppendMenuA(help, MF_SEPARATOR, 0, NULL);
    AppendMenuA(help, MF_STRING, IDM_HELP, "&Keys\tF6");
    AppendMenuA(help, MF_STRING, IDM_ABOUT, "&About " APP_TITLE);

    AppendMenuA(bar, MF_POPUP, (UINT_PTR)console, "&Console");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)view, "&View");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)fuji, "&FujiNet");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)help, "&Help");
    SetMenu(hwnd, bar);
    sync_menu_checks();
}

/* ---- Preferences glue -------------------------------------------------------------------- */

/* The picture options live in the settings: Preferences writes them and says
 * so. */
static void read_display_settings(void)
{
    g_aspect = a52session_get_int(g_session, "aspect", 0) == 0;
    g_smooth = a52session_get_int(g_session, "smooth", 0) != 0;
    a52_display_set_options(g_aspect, g_smooth);
    sync_menu_checks();
}

/* Preferences closed: a console or host row that needed it was applied (and
 * the session power-cycled or restarted) by the session itself. */
static void prefs_closed(int applied)
{
    if (applied == A52_APPLY_POWER) notice("Preferences applied: the console was power-cycled");
    else if (applied == A52_APPLY_RESTART) notice("Preferences applied: the session was restarted");
    a52_controllers_window_refresh();
    update_status();
}

static void show_prefs(void)
{
    a52_prefs_show(g_hwnd, g_session, read_display_settings, prefs_closed);
}

static void open_web_ui(HWND owner)
{
    if (a52session_fujinet_external(g_session)) {
        a52_message(owner, "FujiNet is external: use the web UI of the instance this app dials.", "FujiNet",
                   MB_ICONINFORMATION | MB_OK);
        return;
    }
    if (!a52session_fujinet_running(g_session)) {
        a52_message(owner, "FujiNet is not running.", "FujiNet", MB_ICONINFORMATION | MB_OK);
        return;
    }
    ShellExecuteA(owner, "open", a52session_fujinet_webui_url(g_session), NULL, NULL, SW_SHOWNORMAL);
}

static void show_log(void)
{
    if (a52session_fujinet_external(g_session)) {
        a52_message(g_hwnd, "FujiNet is external: its console output is in the process that is running it.",
                   "FujiNet", MB_ICONINFORMATION | MB_OK);
        return;
    }
    a52_fujilog_show(g_hwnd, g_session);
}

static void show_help(void)
{
    a52_message(g_hwnd,
        "Power Cycle (Shift+F5) is the 5200's power switch: it brings the FujiNet CONFIG back.\n"
        "Open a cartridge image (.a52 .bin .rom .car, or a ZIP of one) or drop it on the window; "
        "the FujiNet cartridge serves it as a game.\n\n"
        "Default keys (the Controllers window remaps them)\n"
        "  Arrow keys              controller 1 stick\n"
        "  Space                   fire\n"
        "  0-9, - (*), = (#)       keypad\n"
        "  F4 / F3 / F5            START / PAUSE / RESET\n"
        "  F6                      this help\n"
        "  F9 / F11 / F12          Controllers / Fullscreen / Debugger\n"
        "  Ctrl+Shift+O / W / Q    Open / Warp / Quit\n\n"
        "Dragging with the mouse in the picture moves controller 1's analog stick.\n"
        "Ports 2-4: assign a gamepad to each in the Controllers window.",
        "Help", MB_ICONINFORMATION | MB_OK);
}

static void show_about(void)
{
    a52_message(g_hwnd,
        APP_TITLE " " A52_VERSION_STRING "\n"
        "Atari 5200 with the FujiNet cartridge built in\n\n"
        "The emulator is atari800 (GPL-2.0+) in its 5200 mode; the default BIOS is the Altirra 5200 OS.\n"
        "The FujiNet firmware and the cartridge: Thomas Cherryhomes and the FujiNet\n"
        "contributors.\n\n"
        "Copyright \xc2\xa9 2026 Thomas Cherryhomes \xe2\x80\x94 GPL-3.0-or-later\n"
        "https://fujinet.online/\n"
        "Issues: https://github.com/FujiNetWIFI/fujinet-go-atari5200-desktop/issues",
        "About " APP_TITLE, MB_ICONINFORMATION | MB_OK);
}

/* ---- the picture as a pointer pad -------------------------------------------------------- */

/* A drag inside the picture moves controller 1's analog stick: the offset
 * from where the button went down, full deflection at a quarter of the
 * picture's size. */
static void drag_stick(int x, int y)
{
    RECT c;
    int span, dx, dy;
    GetClientRect(g_display, &c);
    span = (c.bottom < c.right ? c.bottom : c.right) / 4;
    if (span < 8) span = 8;
    dx = (x - g_drag_x) * 127 / span;
    dy = (y - g_drag_y) * 127 / span;
    a52session_stick_set(g_session, 0, dx < -128 ? -128 : dx > 127 ? 127 : dx,
                         dy < -128 ? -128 : dy > 127 ? 127 : dy);
}

static void drag_end(void)
{
    if (!g_drag) return;
    g_drag = 0;
    a52session_stick_set(g_session, 0, 0, 0);
    if (GetCapture() == g_hwnd) ReleaseCapture();
}

/* ---- window ---------------------------------------------------------------------------------- */

static void toggle_fullscreen(HWND hwnd)
{
    DWORD style = GetWindowLong(hwnd, GWL_STYLE);
    if (!g_fullscreen) {
        MONITORINFO mi;
        memset(&mi, 0, sizeof mi);
        mi.cbSize = sizeof mi;
        g_placement.length = sizeof g_placement;
        GetWindowPlacement(hwnd, &g_placement);
        if (GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLong(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetMenu(hwnd, NULL);
            ShowWindow(g_statusbar, SW_HIDE);
            g_fullscreen = 1;
            SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
    } else {
        SetWindowLong(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        build_menu(hwnd);
        ShowWindow(g_statusbar, SW_SHOW);
        g_fullscreen = 0;
        SetWindowPlacement(hwnd, &g_placement);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    layout_children();
}

/* The app's own keys (key_forward.h's a52_app_key), run from a key press
 * here or from the Controllers window. 1 if wp was one. */
static int app_shortcut(HWND hwnd, WPARAM vk)
{
    const int ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const int shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (vk == VK_F6) { PostMessage(hwnd, WM_COMMAND, IDM_HELP, 0); return 1; }
    if (vk == VK_F9) { a52_controllers_window_toggle(hwnd, g_session); return 1; }
    if (vk == VK_F11) { toggle_fullscreen(hwnd); return 1; }
    if (vk == VK_F12) { a52_debugger_toggle(hwnd, g_session); return 1; }
    if (vk == VK_F5 && shift) { PostMessage(hwnd, WM_COMMAND, IDM_POWER, 0); return 1; }
    if (ctrl && shift && vk == 'O') { PostMessage(hwnd, WM_COMMAND, IDM_OPEN, 0); return 1; }
    if (ctrl && shift && vk == 'W') { PostMessage(hwnd, WM_COMMAND, IDM_WARP, 0); return 1; }
    if (ctrl && shift && vk == 'Q') { PostMessage(hwnd, WM_COMMAND, IDM_EXIT, 0); return 1; }
    if (ctrl && vk == VK_OEM_COMMA) { PostMessage(hwnd, WM_COMMAND, IDM_SETTINGS, 0); return 1; }
    return 0;
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (g_statusbar) SendMessageA(g_statusbar, WM_SIZE, 0, 0);
        layout_children();
        /* minimized: nothing to draw, so the render thread sleeps (the
         * machine, the audio and FujiNet run on) */
        a52_display_set_active(wp != SIZE_MINIMIZED);
        return 0;
    case WM_DRAWITEM:
        if (wp == IDC_STATUSBAR) { draw_status_dot((const DRAWITEMSTRUCT *)lp); return TRUE; }
        break;
    case WM_INITMENUPOPUP:
        sync_console_menu();
        break;
    case WM_ENTERMENULOOP:
        /* The menu takes the keyboard: whatever was held lets go now rather
         * than staying down until the key is pressed again. */
        a52session_release_all(g_session);
        drag_end();
        memset(g_sysact_down, 0, sizeof g_sysact_down);
        break;
    case WM_DPICHANGED: {
        const RECT *r = (const RECT *)lp;
        SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        RECT dr;
        POINT p;
        p.x = (short)LOWORD(lp);
        p.y = (short)HIWORD(lp);
        GetWindowRect(g_display, &dr);
        MapWindowPoints(NULL, hwnd, (POINT *)&dr, 2);
        if (PtInRect(&dr, p)) {
            g_drag = 1;
            g_drag_x = p.x;
            g_drag_y = p.y;
            SetCapture(hwnd);
            SetFocus(hwnd);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_drag) drag_stick((short)LOWORD(lp), (short)HIWORD(lp));
        return 0;
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        drag_end();
        return 0;

    /* WM_SYSKEY* as well: Windows reports Alt -- and every key pressed while
     * it is down -- as a system key. One that drives the console (a bound
     * key) is the console's; the rest (Alt+F4, the menu mnemonics, Alt+Esc)
     * stay Windows'. */
    case WM_KEYDOWN: case WM_SYSKEYDOWN: {
        uint32_t ks;
        int sa;
        if (a52_controllers_map_key(msg, wp, lp)) return 0;
        if (msg == WM_KEYDOWN && a52_app_key(wp)) {
            if (!(lp & (1 << 30))) app_shortcut(hwnd, wp);
            return 0;
        }
        if (msg == WM_SYSKEYDOWN) {
            /* the session's own actions never fire from a system key: Alt+Esc
             * is Windows', not a power cycle */
            if (a52_forward_key(g_session, msg, wp, lp)) return 0;
            break;
        }
        ks = a52_keysym_from_msg(wp, lp);
        if (!ks) break;
        /* The session's own actions (the power-cycle key) first; then the
         * console, through the bindings like every other key. */
        sa = a52session_key_sysaction(g_session, ks);
        if (sa >= 0) {
            if (!(lp & (1 << 30)) && !g_sysact_down[sa]) {
                g_sysact_down[sa] = 1;
                run_sysaction(sa);
                update_status();
            }
            return 0;
        }
        if (a52_forward_key(g_session, msg, wp, lp)) return 0;
        break;
    }
    case WM_KEYUP: case WM_SYSKEYUP: {
        uint32_t ks;
        int sa;
        if (a52_controllers_map_key(msg, wp, lp)) return 0;
        ks = a52_keysym_from_msg(wp, lp);
        if (!ks) break;
        sa = a52session_key_sysaction(g_session, ks);
        if (sa >= 0) { g_sysact_down[sa] = 0; return 0; }
        /* A bound Alt's release stops here, so it does not also put the menu
         * bar into keyboard mode. */
        if (a52_forward_key(g_session, msg, wp, lp)) return 0;
        break;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            a52session_release_all(g_session);
            drag_end();
            memset(g_sysact_down, 0, sizeof g_sysact_down);
        }
        return 0;

    case WM_TIMER:
        if (wp == IDT_STATUS) {
            update_status();
        } else if (wp == IDT_SYSACT) {
            int sa;
            while (a52session_sysaction_take(g_session, &sa)) {
                run_sysaction(sa);
                update_status();
            }
            poll_events();
        } else if (wp == IDT_ENV) {
            const uint64_t frames = a52session_frame_count(g_session);
            if (g_shot_path[0] && frames >= (uint64_t)g_shot_frames) {
                if (!save_png(g_shot_path)) OutputDebugStringA("A52_SCREENSHOT: could not write\n");
                g_shot_path[0] = '\0';
            }
            if (g_quit_frames && frames >= (uint64_t)g_quit_frames && !g_shot_path[0])
                PostMessage(hwnd, WM_CLOSE, 0, 0);
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_OPEN: {
            char path[MAX_PATH];
            if (a52_pick_cart(hwnd, "Open", path, sizeof path)) load_media(path);
            return 0;
        }
        case IDM_IMPORT_SD: a52_import_carts(hwnd, g_session); return 0;
        case IDM_BIOS_FILES: a52_import_bios(hwnd, g_session, A52_BIOS_FILES, -1); return 0;
        case IDM_BIOS_ZIP: a52_import_bios(hwnd, g_session, A52_BIOS_ZIP, -1); return 0;
        case IDM_BIOS_FOLDER: a52_import_bios(hwnd, g_session, A52_BIOS_FOLDER, -1); return 0;
        case IDM_POWER: run_sysaction(A52_SYSACT_POWER); return 0;
        case IDM_PAUSE:
            a52session_set_paused(g_session, !a52session_is_paused(g_session));
            sync_console_menu();
            update_status();
            return 0;
        case IDM_WARP: run_sysaction(A52_SYSACT_WARP); return 0;
        /* the controller's RESET key: the session holds it for a few frames,
         * as a finger would -- the console samples it once a frame */
        case IDM_RESET: a52session_reset(g_session); update_status(); return 0;
        case IDM_CONTROLLERS: a52_controllers_window_toggle(hwnd, g_session); return 0;
        case IDM_DEBUGGER: a52_debugger_toggle(hwnd, g_session); return 0;
        case IDM_FULLSCREEN: toggle_fullscreen(hwnd); return 0;
        case IDM_TV_ASPECT:
            g_aspect = !g_aspect;
            a52session_set_int(g_session, "aspect", g_aspect ? 0 : 1);
            a52_display_set_options(g_aspect, g_smooth);
            sync_menu_checks();
            return 0;
        case IDM_SMOOTH:
            g_smooth = !g_smooth;
            a52session_set_int(g_session, "smooth", g_smooth);
            a52_display_set_options(g_aspect, g_smooth);
            sync_menu_checks();
            return 0;
        case IDM_SETTINGS: show_prefs(); return 0;
        case IDM_FUJINET_LOG: show_log(); return 0;
        case IDM_FUJINET_CONFIG: open_web_ui(hwnd); return 0;
        case IDM_FUJINET_RESTART:
            if (a52session_fujinet_external(g_session) || !a52session_fujinet_running(g_session))
                notice("FujiNet is not the built-in runtime: nothing to restart");
            else if (a52session_fujinet_restart(g_session) != 0)
                notice("Could not restart FujiNet");
            update_status();
            return 0;
        case IDM_HELP: show_help(); return 0;
        case IDM_HELP_TOPICS: a52_help_for(hwnd); return 0;
        case IDM_ABOUT: show_about(); return 0;
        case IDM_EXIT: PostMessage(hwnd, WM_CLOSE, 0, 0); return 0;
        default: break;
        }
        break;

    case WM_DROPFILES: {
        const UINT n = DragQueryFileA((HDROP)wp, 0xFFFFFFFFu, NULL, 0);
        UINT i;
        for (i = 0; i < n; i++) {
            char path[MAX_PATH];
            if (DragQueryFileA((HDROP)wp, i, path, sizeof path)) load_media(path);
        }
        DragFinish((HDROP)wp);
        return 0;
    }

    /* a second launch hands its file to this one */
    case WM_COPYDATA: {
        const COPYDATASTRUCT *cd = (const COPYDATASTRUCT *)lp;
        if (cd && cd->dwData == 1 && cd->lpData && cd->cbData > 1 && cd->cbData < MAX_PATH) {
            char path[MAX_PATH];
            memcpy(path, cd->lpData, cd->cbData);
            path[sizeof path - 1] = '\0';
            if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
            load_media(path);
        }
        return TRUE;
    }

    case WM_CLOSE:
        a52_prefs_abandon();
        break;
    case WM_DESTROY:
        KillTimer(hwnd, IDT_STATUS);
        KillTimer(hwnd, IDT_SYSACT);
        KillTimer(hwnd, IDT_ENV);
        PostQuitMessage(0);
        return 0;
    default: break;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

/* The path on the command line, quotes and all. */
static int command_line_path(const char *cmdline, char *out, size_t outsz)
{
    const char *p = cmdline;
    size_t n;
    if (!cmdline || !*cmdline) return 0;
    if (*p == '"') p++;
    snprintf(out, outsz, "%s", p);
    n = strlen(out);
    while (n && (out[n - 1] == '"' || out[n - 1] == ' ')) out[--n] = '\0';
    return out[0] != '\0';
}

/* One copy of the app: a second launch brings the first forward and hands it
 * the file it was started with. 1 when this process should exit. */
static int hand_over_to_running_instance(const char *path)
{
    HANDLE mutex = CreateMutexA(NULL, FALSE, INSTANCE_MUTEX);
    HWND other;
    if (!mutex || GetLastError() != ERROR_ALREADY_EXISTS)
        return 0;
    other = FindWindowA(WIN_CLASS, NULL);
    if (!other)
        return 0;
    if (path && path[0]) {
        COPYDATASTRUCT cd;
        cd.dwData = 1;
        cd.cbData = (DWORD)strlen(path) + 1;
        cd.lpData = (void *)path;
        SendMessageA(other, WM_COPYDATA, (WPARAM)g_hwnd, (LPARAM)&cd);
    }
    if (IsIconic(other)) ShowWindow(other, SW_RESTORE);
    SetForegroundWindow(other);
    return 1;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    WNDCLASSEX wc;
    MSG msg;
    a52session_start_opts opts;
    RECT want;
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_BAR_CLASSES | ICC_TAB_CLASSES | ICC_STANDARD_CLASSES |
                                             ICC_LISTVIEW_CLASSES | ICC_UPDOWN_CLASS };
    int parts[2] = { DOT_PART_W, -1 };
    char media[MAX_PATH];
    const char *later = NULL;   /* a non-cartridge path from the command line */
    int have_media, dpi, booting_cart = 0;
    (void)prev;

    have_media = command_line_path(cmdline, media, sizeof media);
    if (hand_over_to_running_instance(have_media ? media : NULL))
        return 0;

    InitCommonControlsEx(&icc);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);     /* the folder browser */

    g_session = a52session_new(NULL);
    if (!g_session) {
        MessageBoxA(NULL, "Could not create the session (unusable config or data directories?)",
                    APP_TITLE, MB_ICONERROR | MB_OK);
        return 1;
    }
    g_accent_brush = CreateSolidBrush(RGB((A52SESSION_ACCENT_RGB >> 16) & 0xff,
                                          (A52SESSION_ACCENT_RGB >> 8) & 0xff,
                                          A52SESSION_ACCENT_RGB & 0xff));

    /* "aspect": 0 (the default) the television's 4:3, 1 square pixels --
     * the family's meaning, shared with the other frontends */
    g_aspect = a52session_get_int(g_session, "aspect", 0) == 0;
    g_smooth = a52session_get_int(g_session, "smooth", 0);

    if (getenv("A52_SCREENSHOT")) snprintf(g_shot_path, sizeof g_shot_path, "%s", getenv("A52_SCREENSHOT"));
    if (getenv("A52_SCREENSHOT_FRAMES")) g_shot_frames = atol(getenv("A52_SCREENSHOT_FRAMES"));
    if (getenv("A52_AUTOQUIT_FRAMES")) g_quit_frames = atol(getenv("A52_AUTOQUIT_FRAMES"));

    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = WIN_CLASS;
    wc.hIcon = LoadIcon(inst, MAKEINTRESOURCE(IDI_APPICON));
    wc.hIconSm = wc.hIcon;
    RegisterClassEx(&wc);

    /* Three times the television picture's height (336 x 224), at the chosen
     * shape (896 x 672 at 4:3), plus chrome, at the screen's DPI. */
    dpi = a52_system_dpi();
    SetRect(&want, 0, 0,
            a52_scale((int)(224 * 3 * (g_aspect ? 4.0 / 3.0 : 336.0 / 224.0) + 0.5), dpi),
            a52_scale(224 * 3, dpi));
    AdjustWindowRectEx(&want, WS_OVERLAPPEDWINDOW, TRUE, WS_EX_ACCEPTFILES);
    g_hwnd = CreateWindowEx(WS_EX_ACCEPTFILES, WIN_CLASS, APP_TITLE, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, want.right - want.left,
                            want.bottom - want.top + a52_scale(24, dpi), NULL, NULL, inst, NULL);
    if (!g_hwnd) return 1;
    build_menu(g_hwnd);
    g_statusbar = CreateWindowExA(0, STATUSCLASSNAMEA, "", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                                  0, 0, 0, 0, g_hwnd, (HMENU)(INT_PTR)IDC_STATUSBAR, inst, NULL);
    {
        int scaled[2] = { a52_scale(parts[0], a52_window_dpi(g_hwnd)), -1 };
        SendMessageA(g_statusbar, SB_SETPARTS, 2, (LPARAM)scaled);
    }
    a52_help_set_context(g_hwnd, "main", 0);       /* F1 is the console's RESET here */
    g_display = a52_display_create(g_hwnd, g_session);
    a52_display_set_options(g_aspect, g_smooth);
    layout_children();
    ShowWindow(g_hwnd, show);

    a52session_default_opts(g_session, &opts);
    if (have_media) {
        /* a cartridge boots in place of the remembered one; anything
         * else is routed like a dropped file once the machine is up */
        if (a52session_media_kind(g_session, media) == A52_MEDIA_CARTRIDGE) {
            opts.cart_path = media;
            booting_cart = 1;
        } else {
            later = media;
        }
    }
    if (a52session_start(g_session, &opts) != 0)
        a52_message(g_hwnd, a52session_last_error(g_session), APP_TITLE, MB_ICONWARNING | MB_OK);
    else if (booting_cart && !*a52session_cart_path(g_session))
        later = media;      /* the session booted CONFIG instead: open says why */
    if (later) load_media(later);

    g_pad_generation = a52session_gamepad_generation(g_session);
    g_fuji_generation = a52session_fujinet_generation(g_session);
    SetTimer(g_hwnd, IDT_STATUS, 1000, NULL);
    SetTimer(g_hwnd, IDT_SYSACT, 100, NULL);
    if (g_shot_path[0] || g_quit_frames) SetTimer(g_hwnd, IDT_ENV, 100, NULL);
    update_status();

    if (env_on("A52_OPEN_CONTROLLERS")) a52_controllers_window_toggle(g_hwnd, g_session);
    if (env_on("A52_OPEN_DEBUGGER")) a52_debugger_show(g_hwnd, g_session);
    if (env_on("A52_OPEN_SETTINGS")) show_prefs();

    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        /* Before TranslateMessage, so the sub-windows' keys reach them
         * regardless of which child control has the focus. */
        if (a52_debugger_pretranslate(&msg)) continue;
        if (a52_prefs_pretranslate(&msg)) continue;
        if (a52_help_pretranslate(&msg)) continue;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    a52_display_destroy();
    a52session_stop(g_session);
    a52session_free(g_session);
    DeleteObject(g_accent_brush);
    CoUninitialize();
    return (int)msg.wParam;
}
