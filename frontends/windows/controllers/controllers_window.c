/*
 * The Win32 Controllers window (F9): the Atari 5200's CX52 controllers as the
 * real ones look -- the black wedge with START / PAUSE / RESET along the top,
 * the analog stick in its dish, the silver band, the red side buttons and the
 * recessed 12-key pad -- one per port the console has (four, or two with the
 * 2-port BIOS), drawn from the session's layout (a52session_controls_layout)
 * so every frontend shows the same parts. Under them: the gamepads with the
 * controller each drives, the mouse's port, and Map mode.
 *
 * MOUSE: a press on a key or a fire button holds it for as long as the button
 * is down: the console samples its controllers once per frame, so a value
 * present only for the instant of a click falls between frames. Presses are
 * the raw button-down and button-up, never a click. The mouse is captured on
 * press; letting go anywhere, losing the capture, hiding, Map being armed or
 * deactivating the window releases whatever the pointer holds
 * (a52session_release_pointer). The stick is dragged: the pointer's offset
 * from the dish's centre is the analog deflection (a52session_stick_set),
 * (0,0) on release. The wheel nudges the stick of the controller under the
 * pointer and lets it go again half a second after the last notch.
 *
 * What the console sees held -- keyboard, gamepads and this window together --
 * is polled every 40 ms and drawn as a ring and a 35% fill in the held colour,
 * so the window doubles as an input tester. The keypad and FIRE 2 are common
 * to all controllers (the core scans one of each): a key held on any of them
 * lights on every one, and the line under the controllers says so.
 *
 * FIXED SIZE: laid out once, in a caption-only tool window with no thick frame
 * and no maximize box, so neither Map mode nor a long gamepad name changes its
 * size. (A BIOS change from four ports to two relays it once.)
 *
 * MAP MODE: Map arms a two-step rebind -- click a control (a stick click picks
 * its direction) to pick the target, then press a host key OR a gamepad
 * button; Esc cancels. While armed the controls show what drives them (key /
 * pad button), the picked target is lit and the rest are drawn at 40%, and
 * typing does not reach the console. Clear takes the picked target's key and
 * button away. "Show bindings" shows the same legends outside Map mode. F9,
 * F11 and F12 are the app's and cannot be bound.
 *
 * The art is drawn at twice the size into a memory bitmap and shrunk with
 * HALFTONE (GDI has no anti-aliasing of its own); the text goes on after at
 * the window's own size.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include "controllers_window.h"

/* GET_X_LPARAM / GET_Y_LPARAM live here, not in windows.h. */
#include <windowsx.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../help/help_window.h"
#include "../key_forward.h"
#include "../resource.h"
#include "../win_common.h"

#define PANEL_CLASS   "FujiNetGoA52Controllers"
#define PANEL_STYLE   (WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN)
#define PANEL_EXSTYLE WS_EX_TOOLWINDOW

/* Sizes at 96 dpi; S() scales them to the window's. */
#define UNIT96      13      /* pixels per layout unit */
#define MARGIN      12
#define MIN_CONTENT 540     /* the rows below are this wide at least (two controllers are narrower) */
#define HEAD_H      22
#define ROW_H       26      /* one gamepad's row */
#define ROW_GAP     3
#define PAD_ROWS    3       /* gamepad rows the window has room for */
#define SEG_W       30      /* 1 2 3 4 */
#define SEG_AUTO_W  76      /* Automatic */
#define BTN_H       28
#define HINT_H      20

#define IDT_CAPTURE 1
#define IDT_HELD    2
#define IDT_PADS    3
#define IDT_NUDGE   4

#define NUDGE_STEP  32
#define NUDGE_MS    500

enum { SB_MAP, SB_CLEAR, SB_DEFAULTS, SB_SHOW, SB_HELP, SB_COUNT };
static const int k_btn_id[SB_COUNT] = { IDC_CTRL_MAP, IDC_CTRL_CLEAR, IDC_CTRL_DEFAULTS, IDC_CTRL_SHOW,
                                         IDC_CTRL_HELP };

typedef struct {
    unsigned generation;
    int count, ports, mouse, assign[PAD_ROWS], effective[PAD_ROWS];
} pads_state;

static HWND g_panel, g_parent;
static a52session *g_session;
static int g_dpi = 96;

/* Map mode: -2 idle, -1 armed and waiting for a target, >= 0 waiting for a
 * key or pad button to bind to that target. */
static int g_map_state = -2;
static int g_show_bindings;
static WPARAM g_swallow_vk;     /* the key Map took: its repeats and release too */
static char g_hint[300];
static int g_hover_part = -1;   /* the part under the idle pointer */
static int g_tracking;

/* The layout (units), and where it sits in the client. */
static a52_control g_layout[A52_CONTROLS_MAX];
static int g_numlayout, g_nports = A52_PORTS;
static float g_layw, g_layh;
static double g_unit, g_orgx, g_orgy;

/* What the pointer holds: a pressed part (layout index), a dragged stick. */
static int g_mouse_part = -1;
static int g_nudge[A52_PORTS][2];       /* the wheel's deflection per port */
static int g_nudging;

/* What was last painted. */
static unsigned char g_lit[A52_CONTROLS_MAX];
static int g_stick_shown[A52_PORTS][2];
static pads_state g_pads_shown;
static int g_total_w, g_total_h;

static RECT g_art_rc, g_gp_rc, g_gp_head_rc, g_gp_row_rc[PAD_ROWS], g_mouse_rc;
static RECT g_btn_rc[SB_COUNT], g_hint_rc;

static HFONT g_font, g_bold, g_digit, g_cons, g_band, g_tiny, g_tiny_b;

static void refresh_held(void);

static int S(int v) { return a52_scale(v, g_dpi); }

/* ---- fonts ------------------------------------------------------------------ */

static void free_fonts(void)
{
    HFONT *const slots[] = { &g_font, &g_bold, &g_digit, &g_cons, &g_band, &g_tiny, &g_tiny_b };
    size_t idx;
    for (idx = 0; idx < sizeof slots / sizeof slots[0]; idx++)
        if (*slots[idx]) { DeleteObject(*slots[idx]); *slots[idx] = NULL; }
}

static HFONT face_font(int px, int weight, int italic)
{
    return CreateFontW(-MulDiv(px, g_dpi, 96), 0, 0, 0, weight, italic, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                       L"Segoe UI");
}

static void make_fonts(void)
{
    free_fonts();
    g_font = a52_make_font(g_dpi, 0, FW_NORMAL, NULL);
    g_bold = a52_make_font(g_dpi, 0, FW_BOLD, NULL);
    g_digit = face_font(15, FW_BOLD, 0);        /* the keypad's digits */
    g_cons = face_font(10, FW_BOLD, 1);         /* START / PAUSE / RESET: bold italic */
    g_band = face_font(9, FW_BOLD, 0);          /* ATARI 5200 */
    g_tiny = a52_make_font(g_dpi, 9, FW_NORMAL, NULL);      /* binding legends */
    g_tiny_b = a52_make_font(g_dpi, 9, FW_BOLD, NULL);
}

/* ---- text -------------------------------------------------------------------- */

/* UTF-8 (key legends, gamepad names) through DrawTextW. */
static int text_out(HDC hdc, const char *utf8, RECT *rect, UINT fmt)
{
    wchar_t wide[256];
    if (!utf8 || !*utf8) return 0;
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, (int)(sizeof wide / sizeof wide[0])) <= 0) return 0;
    return DrawTextW(hdc, wide, -1, rect, fmt | DT_NOPREFIX);
}

static void text_line(HDC hdc, const char *utf8, const RECT *rect, UINT fmt)
{
    RECT copy = *rect;
    text_out(hdc, utf8, &copy, fmt | DT_SINGLELINE | DT_VCENTER);
}

/* ---- colours ----------------------------------------------------------------- */

static COLORREF rgb_of(unsigned val)
{
    return RGB((val >> 16) & 0xff, (val >> 8) & 0xff, val & 0xff);
}

#define NOFILL ((COLORREF)0xFFFFFFFFu)

static COLORREF mix_color(COLORREF start, COLORREF finish, double frac)
{
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    return RGB((int)(GetRValue(start) + (GetRValue(finish) - GetRValue(start)) * frac + 0.5),
               (int)(GetGValue(start) + (GetGValue(finish) - GetGValue(start)) * frac + 0.5),
               (int)(GetBValue(start) + (GetBValue(finish) - GetBValue(start)) * frac + 0.5));
}

/* An unpicked part in Map mode: 40% of its own colours over the window's. */
static COLORREF tone(COLORREF col, int dim)
{
    return dim ? mix_color(col, GetSysColor(COLOR_BTNFACE), 0.6) : col;
}

/* ---- drawing primitives ---------------------------------------------------------
 * The art is drawn at g_art_k times the window's size into a bitmap that holds
 * the pixel rectangle starting at (g_art_x, g_art_y). */
static int g_art_k = 1, g_art_x, g_art_y, g_art_rows;

static int art_xpos(double val) { return (int)floor((val - g_art_x) * g_art_k + 0.5); }
static int art_ypos(double val) { return (int)floor((val - g_art_y) * g_art_k + 0.5); }
static int art_len(double val) { return (int)floor(val * g_art_k + 0.5); }

static POINT art_pt(double xpos, double ypos)
{
    POINT pnt;
    pnt.x = art_xpos(xpos);
    pnt.y = art_ypos(ypos);
    return pnt;
}

static void poly(HDC hdc, const POINT *pnts, int num, COLORREF fill, COLORREF line, double width)
{
    HBRUSH brush = fill == NOFILL ? (HBRUSH)GetStockObject(NULL_BRUSH) : CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, width > 0 ? art_len(width) : 0, line);
    HGDIOBJ oldb = SelectObject(hdc, brush);
    HGDIOBJ oldp = SelectObject(hdc, width > 0 ? (HGDIOBJ)pen : GetStockObject(NULL_PEN));
    Polygon(hdc, pnts, num);
    SelectObject(hdc, oldp);
    SelectObject(hdc, oldb);
    DeleteObject(pen);
    if (fill != NOFILL) DeleteObject(brush);
}

static void ellipse(HDC hdc, double ctrx, double ctry, double radx, double rady, COLORREF fill, COLORREF line,
                    double width)
{
    HBRUSH brush = fill == NOFILL ? (HBRUSH)GetStockObject(NULL_BRUSH) : CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, width > 0 ? art_len(width) : 0, line);
    HGDIOBJ oldb = SelectObject(hdc, brush);
    HGDIOBJ oldp = SelectObject(hdc, width > 0 ? (HGDIOBJ)pen : GetStockObject(NULL_PEN));
    Ellipse(hdc, art_xpos(ctrx - radx), art_ypos(ctry - rady), art_xpos(ctrx + radx), art_ypos(ctry + rady));
    SelectObject(hdc, oldp);
    SelectObject(hdc, oldb);
    DeleteObject(pen);
    if (fill != NOFILL) DeleteObject(brush);
}

static void rrect(HDC hdc, double xpos, double ypos, double width, double height, double radius, COLORREF fill,
                  COLORREF line, double linew)
{
    HBRUSH brush = fill == NOFILL ? (HBRUSH)GetStockObject(NULL_BRUSH) : CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, linew > 0 ? art_len(linew) : 0, line);
    HGDIOBJ oldb = SelectObject(hdc, brush);
    HGDIOBJ oldp = SelectObject(hdc, linew > 0 ? (HGDIOBJ)pen : GetStockObject(NULL_PEN));
    RoundRect(hdc, art_xpos(xpos), art_ypos(ypos), art_xpos(xpos + width), art_ypos(ypos + height),
              art_len(radius * 2), art_len(radius * 2));
    SelectObject(hdc, oldp);
    SelectObject(hdc, oldb);
    DeleteObject(pen);
    if (fill != NOFILL) DeleteObject(brush);
}

static void line_seg(HDC hdc, double x0, double y0, double x1, double y1, COLORREF col, double width)
{
    HPEN pen = CreatePen(PS_SOLID, art_len(width) > 0 ? art_len(width) : 1, col);
    HGDIOBJ oldp = SelectObject(hdc, pen);
    MoveToEx(hdc, art_xpos(x0), art_ypos(y0), NULL);
    LineTo(hdc, art_xpos(x1), art_ypos(y1));
    SelectObject(hdc, oldp);
    DeleteObject(pen);
}

/* A rounded rectangle filled with a vertical gradient, then outlined. */
static void grad_rrect(HDC hdc, double xpos, double ypos, double width, double height, double radius,
                       COLORREF top, COLORREF bottom, COLORREF edge, double edgew)
{
    const int left = art_xpos(xpos), right = art_xpos(xpos + width);
    const int first = art_ypos(ypos), last = art_ypos(ypos + height);
    HRGN clip = CreateRoundRectRgn(left, first, right + 1, last + 1, art_len(radius * 2), art_len(radius * 2));
    int row;
    HGDIOBJ oldb;
    SelectClipRgn(hdc, clip);
    oldb = SelectObject(hdc, GetStockObject(DC_BRUSH));
    for (row = first < 0 ? 0 : first; row <= last && row < g_art_rows; row++) {
        SetDCBrushColor(hdc, mix_color(top, bottom, last > first ? (double)(row - first) / (last - first) : 0));
        PatBlt(hdc, left, row, right - left + 1, 1, PATCOPY);
    }
    SelectObject(hdc, oldb);
    SelectClipRgn(hdc, NULL);
    DeleteObject(clip);
    if (edgew > 0) rrect(hdc, xpos, ypos, width, height, radius, NOFILL, edge, edgew);
}

/* The pixel rectangle of a layout part. */
static void part_box(const a52_control *ctl, double *xpos, double *ypos, double *width, double *height)
{
    *xpos = g_orgx + (double)ctl->x * g_unit;
    *ypos = g_orgy + (double)ctl->y * g_unit;
    *width = (double)ctl->w * g_unit;
    *height = (double)ctl->h * g_unit;
}

static void part_rect(const a52_control *ctl, RECT *rect)
{
    double xpos, ypos, width, height;
    part_box(ctl, &xpos, &ypos, &width, &height);
    SetRect(rect, (int)floor(xpos), (int)floor(ypos), (int)ceil(xpos + width), (int)ceil(ypos + height));
}

/* ---- what a part shows ---------------------------------------------------------- */

static int binding_mode(void)
{
    return g_map_state != -2 || g_show_bindings;
}

static int pressable(const a52_control *ctl)
{
    return ctl->kind == A52_CTL_STICK || ctl->kind == A52_CTL_FIRE || ctl->kind == A52_CTL_CONSOLE_KEY ||
           ctl->kind == A52_CTL_KEYCAP;
}

/* The target is one of a port's four stick directions. */
static int is_dir_of(int target, int port)
{
    return target >= A52_TARGET_PORT(port, A52_ACT_UP) && target <= A52_TARGET_PORT(port, A52_ACT_RIGHT);
}

/* Shown held: what the console sees. In Map mode the picked target is the one lit. */
static int part_lit(int idx)
{
    const a52_control *ctl = &g_layout[idx];
    if (g_map_state != -2) {
        if (g_map_state < 0 || !pressable(ctl)) return 0;
        if (ctl->kind == A52_CTL_STICK) return is_dir_of(g_map_state, ctl->port);
        return ctl->target == g_map_state;
    }
    return idx < A52_CONTROLS_MAX && g_lit[idx];
}

/* Map mode with a target picked: every other control is drawn at 40%. */
static int part_dim(const a52_control *ctl)
{
    if (g_map_state < 0 || !pressable(ctl)) return 0;
    if (ctl->kind == A52_CTL_STICK) return !is_dir_of(g_map_state, ctl->port);
    return ctl->target != g_map_state;
}

/* What drives a target, as a "key" and a "pad" line ("" when unbound). */
static void binding_lines(int target, char *key, int keysz, char *pad, int padsz)
{
    a52_binding bind;
    key[0] = pad[0] = '\0';
    if (target < 0) return;
    bind = a52session_binding_get(g_session, target);
    a52session_keysym_name(bind.keysym, key, keysz);
    if (bind.button != A52_PAD_BTN_NONE) snprintf(pad, (size_t)padsz, "%s", a52_pad_button_name(bind.button));
    /* the keys are small: "Keypad 5" reads "KP 5" */
    if (strncmp(key, "Keypad ", 7) == 0) {
        char shortname[64];
        snprintf(shortname, sizeof shortname, "KP %.50s", key + 7);
        snprintf(key, (size_t)keysz, "%s", shortname);
    }
}

/* ---- the art -------------------------------------------------------------------- */

static const a52_control *body_of(int port)
{
    int idx;
    for (idx = 0; idx < g_numlayout; idx++)
        if (g_layout[idx].kind == A52_CTL_BODY && g_layout[idx].port == port) return &g_layout[idx];
    return NULL;
}

/* How far the wedge's edge is in from the box's side at pixel row ypx: none at
 * the stick end, half the taper at the keypad end. */
static double body_inset(const a52_control *body, double ypx)
{
    double xpos, ypos, width, height, frac;
    part_box(body, &xpos, &ypos, &width, &height);
    frac = (ypx - ypos) / height;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    return (body->w - A52_BODY_BOTTOM_W) / 2.0 * g_unit * frac;
}

static void held_style(int lit, COLORREF base, COLORREF *fill, COLORREF *edge, double *edgew)
{
    if (lit) {
        *fill = mix_color(base, rgb_of(A52SESSION_HELD_RGB), 0.35);
        *edge = rgb_of(A52SESSION_HELD_RGB);
        *edgew = 1.8;
    }
}

static void art_body(HDC hdc, const a52_control *ctl)
{
    double xpos, ypos, width, height;
    const double cham = g_unit * 0.9, cham2 = g_unit * 0.5;
    const double ins = (ctl->w - A52_BODY_BOTTOM_W) / 2.0 * g_unit;
    POINT pts[8];
    part_box(ctl, &xpos, &ypos, &width, &height);
    pts[0] = art_pt(xpos, ypos + cham);
    pts[1] = art_pt(xpos + cham, ypos);
    pts[2] = art_pt(xpos + width - cham, ypos);
    pts[3] = art_pt(xpos + width, ypos + cham);
    pts[4] = art_pt(xpos + width - ins, ypos + height - cham2);
    pts[5] = art_pt(xpos + width - ins - cham2, ypos + height);
    pts[6] = art_pt(xpos + ins + cham2, ypos + height);
    pts[7] = art_pt(xpos + ins, ypos + height - cham2);
    poly(hdc, pts, 8, rgb_of(A52SESSION_CASE_RGB), rgb_of(A52SESSION_CASE_SHADOW_RGB), 1.4);
    /* the bevel: a light edge along the top and the left, a dark one along the right */
    line_seg(hdc, xpos + cham, ypos + g_unit * 0.12, xpos + width - cham, ypos + g_unit * 0.12,
             rgb_of(A52SESSION_CASE_BEVEL_RGB), 1.0);
    line_seg(hdc, xpos + g_unit * 0.12, ypos + cham, xpos + ins + g_unit * 0.12, ypos + height - cham2,
             rgb_of(A52SESSION_CASE_BEVEL_RGB), 0.8);
}

static void art_band(HDC hdc, const a52_control *ctl)
{
    const a52_control *body = body_of(ctl->port);
    double xpos, ypos, width, height, fx;
    int row;
    HGDIOBJ oldb;
    const int first = art_ypos(g_orgy + (double)ctl->y * g_unit);
    const int last = art_ypos(g_orgy + (double)(ctl->y + ctl->h) * g_unit);
    part_box(ctl, &xpos, &ypos, &width, &height);
    if (!body) return;
    oldb = SelectObject(hdc, GetStockObject(DC_BRUSH));
    for (row = first < 0 ? 0 : first; row <= last && row < g_art_rows; row++) {
        const double wy = g_art_y + (double)row / g_art_k;
        const double inset = body_inset(body, wy);
        const int left = art_xpos(xpos + inset), right = art_xpos(xpos + width - inset);
        SetDCBrushColor(hdc, mix_color(rgb_of(A52SESSION_BAND_HI_RGB), rgb_of(A52SESSION_BAND_LO_RGB),
                                        last > first ? (double)(row - first) / (last - first) : 0));
        PatBlt(hdc, left, row, right - left, 1, PATCOPY);
    }
    SelectObject(hdc, oldb);
    /* the rainbow line under the text */
    {
        static const unsigned rainbow[4] = { A52SESSION_RAINBOW_ORANGE_RGB, A52SESSION_RAINBOW_YELLOW_RGB,
                                             A52SESSION_RAINBOW_BLUE_RGB, A52SESSION_RAINBOW_GREEN_RGB };
        const double rx = xpos + g_unit * 3.2, rw = g_unit * 6.0 / 4;
        int idx;
        for (idx = 0; idx < 4; idx++)
            rrect(hdc, rx + idx * rw, ypos + g_unit * 1.72, rw + 0.5, g_unit * 0.2, 0, rgb_of(rainbow[idx]),
                  rgb_of(rainbow[idx]), 0);
    }
    /* the Atari fuji, embossed at the left: a stem and two flared bars */
    fx = xpos + g_unit * 1.55;
    {
        const COLORREF ink = rgb_of(A52SESSION_BAND_TEXT_RGB);
        const double top = ypos + g_unit * 0.35, bot = ypos + height - g_unit * 0.3, wid = g_unit * 0.2;
        POINT pts[4];
        pts[0] = art_pt(fx - wid / 2, top); pts[1] = art_pt(fx + wid / 2, top);
        pts[2] = art_pt(fx + wid / 2, bot); pts[3] = art_pt(fx - wid / 2, bot);
        poly(hdc, pts, 4, ink, ink, 0);
        pts[0] = art_pt(fx - wid * 1.4, top); pts[1] = art_pt(fx - wid * 0.5, top);
        pts[2] = art_pt(fx - g_unit * 0.55, bot); pts[3] = art_pt(fx - g_unit * 0.55 - wid * 0.9, bot);
        poly(hdc, pts, 4, ink, ink, 0);
        pts[0] = art_pt(fx + wid * 1.4, top); pts[1] = art_pt(fx + wid * 0.5, top);
        pts[2] = art_pt(fx + g_unit * 0.55, bot); pts[3] = art_pt(fx + g_unit * 0.55 + wid * 0.9, bot);
        poly(hdc, pts, 4, ink, ink, 0);
    }
}

static void art_well(HDC hdc, const a52_control *ctl)
{
    const a52_control *body = body_of(ctl->port);
    double xpos, ypos, width, height;
    POINT pts[4];
    double inset_top, inset_bot;
    part_box(ctl, &xpos, &ypos, &width, &height);
    /* the well follows the wedge's taper: its sides keep a quarter unit off the edge */
    inset_top = body ? body_inset(body, ypos) : 0;
    inset_bot = body ? body_inset(body, ypos + height) : 0;
    {
        const double wl = g_orgx + 0, keep = g_unit * 0.25;
        double left_t = xpos, left_b = xpos, right_t = xpos + width, right_b = xpos + width;
        const double bx = body ? g_orgx + (double)body->x * g_unit : wl;
        const double bw = body ? (double)body->w * g_unit : width;
        if (bx + inset_top + keep > left_t) left_t = bx + inset_top + keep;
        if (bx + inset_bot + keep > left_b) left_b = bx + inset_bot + keep;
        if (bx + bw - inset_top - keep < right_t) right_t = bx + bw - inset_top - keep;
        if (bx + bw - inset_bot - keep < right_b) right_b = bx + bw - inset_bot - keep;
        pts[0] = art_pt(left_t, ypos); pts[1] = art_pt(right_t, ypos);
        pts[2] = art_pt(right_b, ypos + height); pts[3] = art_pt(left_b, ypos + height);
    }
    poly(hdc, pts, 4, rgb_of(A52SESSION_WELL_RGB), rgb_of(A52SESSION_CASE_SHADOW_RGB), 1.2);
}

static void art_key(HDC hdc, int idx)
{
    const a52_control *ctl = &g_layout[idx];
    double xpos, ypos, width, height;
    const int lit = part_lit(idx), dim = part_dim(ctl);
    const int pressed = idx == g_mouse_part && g_map_state == -2;
    COLORREF top = mix_color(rgb_of(A52SESSION_KEY_RGB), rgb_of(A52SESSION_CASE_BEVEL_RGB), 0.55);
    COLORREF bottom = rgb_of(A52SESSION_KEY_RGB);
    COLORREF edge = rgb_of(A52SESSION_CASE_SHADOW_RGB);
    double edgew = 1.0, shift = pressed ? 1.0 : 0.0;
    part_box(ctl, &xpos, &ypos, &width, &height);
    held_style(lit, bottom, &bottom, &edge, &edgew);
    if (lit) top = mix_color(top, rgb_of(A52SESSION_HELD_RGB), 0.35);
    /* the cap's lower edge, then the cap */
    rrect(hdc, xpos, ypos + g_unit * 0.15, width, height - g_unit * 0.15, g_unit * 0.22,
          tone(rgb_of(A52SESSION_CASE_SHADOW_RGB), dim), tone(rgb_of(A52SESSION_CASE_SHADOW_RGB), dim), 0);
    grad_rrect(hdc, xpos, ypos + shift, width, height - g_unit * 0.18 - shift, g_unit * 0.22, tone(top, dim),
               tone(bottom, dim), tone(edge, dim), edgew);
}

static void art_fire(HDC hdc, int idx)
{
    const a52_control *ctl = &g_layout[idx];
    double xpos, ypos, width, height;
    const int lit = part_lit(idx), dim = part_dim(ctl);
    const COLORREF base = rgb_of(A52SESSION_FIRE_RGB);
    COLORREF top = rgb_of(A52SESSION_FIRE_HI_RGB), bottom = base;
    COLORREF edge = mix_color(base, RGB(0, 0, 0), 0.55);
    double edgew = 1.0;
    part_box(ctl, &xpos, &ypos, &width, &height);
    held_style(lit, base, &bottom, &edge, &edgew);
    if (lit) top = mix_color(top, rgb_of(A52SESSION_HELD_RGB), 0.35);
    grad_rrect(hdc, xpos, ypos, width, height, g_unit * 0.3, tone(top, dim), tone(bottom, dim), tone(edge, dim),
               edgew);
}

static void art_dish(HDC hdc, const a52_control *ctl)
{
    double xpos, ypos, width, height;
    const int dim = g_map_state >= 0 && !is_dir_of(g_map_state, ctl->port);
    part_box(ctl, &xpos, &ypos, &width, &height);
    {
        const double ctrx = xpos + width / 2, ctry = ypos + height / 2, rad = width / 2;
        /* the chamfered rim, bright on the upper left and dark on the lower right */
        ellipse(hdc, ctrx, ctry, rad, rad, tone(rgb_of(A52SESSION_DISH_RIM_LO_RGB), dim),
                tone(rgb_of(A52SESSION_DISH_RIM_HI_RGB), dim), 1.0);
        ellipse(hdc, ctrx, ctry, rad * 0.9, rad * 0.9, tone(rgb_of(A52SESSION_CASE_SHADOW_RGB), dim),
                tone(rgb_of(A52SESSION_DISH_RIM_LO_RGB), dim), 0.8);
    }
}

/* The stick: the rubber boot with its rings, the shaft and the ribbed cap
 * leaning the way the stick reads (a gamepad's analog stick, a mouse drag or
 * the directions held). */
static void art_stick(HDC hdc, int idx)
{
    const a52_control *ctl = &g_layout[idx];
    double xpos, ypos, width, height, dishr;
    int sx = 0, sy = 0;
    const int lit = part_lit(idx), dim = part_dim(ctl);
    const unsigned char held = idx < A52_CONTROLS_MAX ? g_lit[idx] : 0;
    part_box(ctl, &xpos, &ypos, &width, &height);
    dishr = g_unit * 3.4;       /* the dish this drag region sits in */
    if (g_map_state == -2) {
        a52session_stick_get(g_session, ctl->port, &sx, &sy);
        g_stick_shown[ctl->port][0] = sx;
        g_stick_shown[ctl->port][1] = sy;
    }
    {
        const double ctrx = xpos + width / 2, ctry = ypos + height / 2;
        const double dx = sx / 127.0, dy = sy / 127.0;
        const double bx = ctrx + dx * dishr * 0.10, by = ctry + dy * dishr * 0.10;
        const double cx = ctrx + dx * dishr * 0.45, cy = ctry + dy * dishr * 0.45;
        const COLORREF boot = tone(rgb_of(A52SESSION_STICK_BOOT_RGB), dim);
        const COLORREF shaft = tone(rgb_of(A52SESSION_STICK_SHAFT_RGB), dim);
        const COLORREF rim_hi = tone(rgb_of(A52SESSION_DISH_RIM_HI_RGB), dim);
        const COLORREF rim_lo = tone(rgb_of(A52SESSION_DISH_RIM_LO_RGB), dim);
        COLORREF cap_fill = shaft, cap_edge = rim_hi;
        double capw = 1.0;

        ellipse(hdc, bx, by, dishr * 0.7, dishr * 0.7, boot, rim_lo, 0.8);
        ellipse(hdc, bx, by, dishr * 0.56, dishr * 0.56, NOFILL, rim_lo, 0.8);      /* the boot's rings */
        ellipse(hdc, (bx + cx) / 2, (by + cy) / 2, dishr * 0.43, dishr * 0.43, NOFILL, rim_lo, 0.8);
        if (!binding_mode()) {
            /* the shaft, then the cap on top, tilted toward the lean */
            line_seg(hdc, bx, by, cx, cy, shaft, dishr * 0.2);
            held_style(lit || held, shaft, &cap_fill, &cap_edge, &capw);
            ellipse(hdc, cx, cy, dishr * 0.28, dishr * 0.28, cap_fill, cap_edge, capw);
            ellipse(hdc, cx, cy, dishr * 0.16, dishr * 0.16, NOFILL, rim_lo, 0.8);   /* the cap's rings */
            ellipse(hdc, cx, cy, dishr * 0.07, dishr * 0.07, rim_lo, rim_lo, 0);
        }
        if ((lit || held) && binding_mode())
            ellipse(hdc, ctrx, ctry, dishr * 0.98, dishr * 0.98, NOFILL, rgb_of(A52SESSION_HELD_RGB), 1.8);
        else if (lit || held)
            ellipse(hdc, ctrx, ctry, dishr * 0.98, dishr * 0.98, NOFILL, rgb_of(A52SESSION_HELD_RGB), 1.4);
    }
}

static void art_all(HDC hdc)
{
    int idx;
    for (idx = 0; idx < g_numlayout; idx++) {
        const a52_control *ctl = &g_layout[idx];
        double xpos, ypos, width, height;
        part_box(ctl, &xpos, &ypos, &width, &height);
        switch (ctl->kind) {
        case A52_CTL_CABLE:
            rrect(hdc, xpos, ypos, width, height + g_unit * 0.3, g_unit * 0.25, rgb_of(A52SESSION_CASE_RGB),
                  rgb_of(A52SESSION_CASE_SHADOW_RGB), 1.0);
            break;
        case A52_CTL_BODY: art_body(hdc, ctl); break;
        case A52_CTL_BAND: art_band(hdc, ctl); break;
        case A52_CTL_DISH: art_dish(hdc, ctl); break;
        case A52_CTL_STICK: art_stick(hdc, idx); break;
        case A52_CTL_WELL: art_well(hdc, ctl); break;
        case A52_CTL_SLOT:
            rrect(hdc, xpos, ypos, width, height, g_unit * 0.12, tone(rgb_of(A52SESSION_SLOT_RGB), g_map_state >= 0),
                  rgb_of(A52SESSION_CASE_SHADOW_RGB), 0.6);
            break;
        case A52_CTL_FIRE: art_fire(hdc, idx); break;
        case A52_CTL_CONSOLE_KEY:
        case A52_CTL_KEYCAP: art_key(hdc, idx); break;
        default: break;
        }
    }
}

/* ---- the text -------------------------------------------------------------------- */

/* The legend colour of a console key. */
static COLORREF console_ink(const a52_control *ctl)
{
    if (ctl->target == A52_TARGET_KEY(A52_KEY_START)) return rgb_of(A52SESSION_START_RGB);
    if (ctl->target == A52_TARGET_KEY(A52_KEY_PAUSE)) return rgb_of(A52SESSION_PAUSE_RGB);
    return rgb_of(A52SESSION_RESET_RGB);
}

/* What a pressable face says when it shows its bindings: the key over the pad
 * button, each on a line of its own (centred, cut with an ellipsis). */
static void binding_text(HDC hdc, int target, const RECT *face)
{
    char key[64], pad[64];
    RECT upper = *face, lower = *face;
    const int mid = (face->top + face->bottom) / 2;
    binding_lines(target, key, sizeof key, pad, sizeof pad);
    if (!key[0] && !pad[0]) snprintf(key, sizeof key, "%s", "\xe2\x80\x94");
    if (!pad[0]) {
        text_line(hdc, key, face, DT_CENTER | DT_END_ELLIPSIS);
        return;
    }
    if (!key[0]) snprintf(key, sizeof key, "%s", "\xe2\x80\x94");
    upper.bottom = mid + S(1);
    lower.top = mid - S(1);
    text_line(hdc, key, &upper, DT_CENTER | DT_END_ELLIPSIS);
    text_line(hdc, pad, &lower, DT_CENTER | DT_END_ELLIPSIS);
}

/* The four directions' legends round the dish in Map mode. */
static void stick_legends(HDC hdc, const a52_control *ctl, int dim)
{
    static const int acts[4] = { A52_ACT_UP, A52_ACT_DOWN, A52_ACT_LEFT, A52_ACT_RIGHT };
    RECT box;
    int idx;
    part_rect(ctl, &box);
    {
        const int ctrx = (box.left + box.right) / 2, ctry = (box.top + box.bottom) / 2;
        const int rad = (box.right - box.left) / 2 + (int)(g_unit * 0.6);
        const int halfw = (int)(g_unit * 1.45), halfh = S(7);
        for (idx = 0; idx < 4; idx++) {
            const int target = A52_TARGET_PORT(ctl->port, acts[idx]);
            RECT leg;
            int centrex = ctrx, centrey = ctry;
            if (acts[idx] == A52_ACT_UP) centrey = ctry - rad * 62 / 100;
            else if (acts[idx] == A52_ACT_DOWN) centrey = ctry + rad * 62 / 100;
            else if (acts[idx] == A52_ACT_LEFT) centrex = ctrx - rad * 62 / 100;
            else centrex = ctrx + rad * 62 / 100;
            SetRect(&leg, centrex - halfw, centrey - halfh, centrex + halfw, centrey + halfh);
            SetTextColor(hdc, g_map_state == target ? rgb_of(A52SESSION_HELD_RGB)
                                                    : tone(RGB(0xE0, 0xE0, 0xE4), dim && g_map_state != target));
            SelectObject(hdc, g_map_state == target ? g_tiny_b : g_tiny);
            binding_text(hdc, target, &leg);
        }
    }
}

static void text_parts(HDC hdc)
{
    int idx;
    SetBkMode(hdc, TRANSPARENT);
    for (idx = 0; idx < g_numlayout; idx++) {
        const a52_control *ctl = &g_layout[idx];
        RECT rect;
        const int lit = part_lit(idx), dim = part_dim(ctl);
        const int pressed = idx == g_mouse_part && g_map_state == -2;
        part_rect(ctl, &rect);
        switch (ctl->kind) {
        case A52_CTL_CAPTION:
            if (!strcmp(ctl->label, "ATARI 5200")) {
                SelectObject(hdc, g_band);
                SetTextColor(hdc, rgb_of(A52SESSION_BAND_TEXT_RGB));
                text_line(hdc, ctl->label, &rect, DT_CENTER);
            } else {
                SelectObject(hdc, g_bold);
                SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
                text_line(hdc, ctl->label, &rect, DT_CENTER);
            }
            break;
        case A52_CTL_NOTE:
            SelectObject(hdc, g_font);
            SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
            text_line(hdc, ctl->label, &rect, DT_CENTER | DT_END_ELLIPSIS);
            break;
        case A52_CTL_STICK:
            if (binding_mode()) stick_legends(hdc, ctl, dim);
            break;
        case A52_CTL_FIRE: {
            RECT face = rect;
            if (!binding_mode()) break;
            SetTextColor(hdc, tone(RGB(255, 255, 255), dim));
            SelectObject(hdc, g_tiny);
            binding_text(hdc, ctl->target, &face);
            break;
        }
        case A52_CTL_CONSOLE_KEY:
        case A52_CTL_KEYCAP: {
            RECT face = rect;
            face.bottom -= S(2);
            if (pressed) OffsetRect(&face, 0, 1);
            if (binding_mode()) {
                SetTextColor(hdc, lit ? RGB(255, 255, 255) : tone(RGB(0xE8, 0xE8, 0xEC), dim));
                SelectObject(hdc, g_tiny);
                binding_text(hdc, ctl->target, &face);
            } else {
                if (ctl->kind == A52_CTL_KEYCAP) {
                    SelectObject(hdc, g_digit);
                    SetTextColor(hdc, rgb_of(A52SESSION_LEGEND_RGB));
                } else {
                    SelectObject(hdc, g_cons);
                    SetTextColor(hdc, console_ink(ctl));
                }
                text_line(hdc, ctl->label, &face, DT_CENTER);
            }
            break;
        }
        default:
            break;
        }
    }
}

/* ---- the strip: gamepads, mouse, Map, hint ------------------------------------------ */

static void draw_face(HDC hdc, const RECT *bounds, int pushed, int accent, int inactive)
{
    RECT rect = *bounds;
    if (accent) {
        HBRUSH back = CreateSolidBrush(rgb_of(A52SESSION_ACCENT_RGB));
        HBRUSH edge = CreateSolidBrush(rgb_of(A52SESSION_ACCENT_DARK_RGB));
        FillRect(hdc, &rect, back);
        FrameRect(hdc, &rect, edge);
        DeleteObject(back);
        DeleteObject(edge);
        SetTextColor(hdc, RGB(255, 255, 255));
    } else {
        DrawFrameControl(hdc, &rect, DFC_BUTTON,
                         DFCS_BUTTONPUSH | (pushed ? DFCS_PUSHED : 0) | (inactive ? DFCS_INACTIVE : 0));
        SetTextColor(hdc, GetSysColor(inactive ? COLOR_GRAYTEXT : COLOR_BTNTEXT));
    }
}

static const char *strip_label(int btn)
{
    switch (btn) {
    case SB_MAP: return g_map_state == -2 ? "Map" : "Done";
    case SB_CLEAR: return "Clear";
    case SB_DEFAULTS: return "Defaults";
    case SB_SHOW: return "Show bindings";
    default: return "Help";
    }
}

/* The choice at the right of a row: with has_auto segment 0 is Automatic and
 * segment n the controller n; without, segment n is the controller n + 1. */
static void segment_rect(const RECT *row, int seg, int has_auto, RECT *rect)
{
    const int gap = S(4), first = has_auto ? 0 : 1;
    const int total = (has_auto ? S(SEG_AUTO_W) + gap : 0) + g_nports * (S(SEG_W) + gap) - gap;
    int left = row->right - total, idx;
    for (idx = first; idx < seg; idx++)
        left += (idx == 0 ? S(SEG_AUTO_W) : S(SEG_W)) + gap;
    SetRect(rect, left, row->top, left + (seg == 0 ? S(SEG_AUTO_W) : S(SEG_W)), row->bottom - 1);
}

static int mouse_port(void)
{
    int port = a52session_get_int(g_session, "mouse_port", 0);
    if (port < 0) port = 0;
    if (port >= g_nports) port = g_nports - 1;
    return port;
}

/* One line per gamepad: its name and the controller it drives now, then the
 * Automatic / 1..N choice, the current one in the accent. */
static void paint_gamepads(HDC hdc)
{
    const int num = a52session_gamepad_count(g_session);
    int idx, seg;

    SelectObject(hdc, g_bold);
    SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
    text_line(hdc, "Gamepads", &g_gp_head_rc, DT_LEFT);
    SelectObject(hdc, g_font);
    SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
    if (num == 0) {
        text_line(hdc, "No gamepads connected - plug one in", &g_gp_row_rc[0], DT_LEFT);
    } else {
        if (num > PAD_ROWS) {
            char more[48];
            snprintf(more, sizeof more, "%d more not shown", num - PAD_ROWS);
            text_line(hdc, more, &g_gp_head_rc, DT_RIGHT);
        }
        for (idx = 0; idx < num && idx < PAD_ROWS; idx++) {
            const int assign = a52session_gamepad_assignment(g_session, idx);
            const int effective = a52session_gamepad_effective_port(g_session, idx);
            const int chosen = assign < 0 || assign >= g_nports ? 0 : assign + 1;
            char name[128], line[200];
            RECT name_rc, first;

            segment_rect(&g_gp_row_rc[idx], 0, 1, &first);
            name_rc = g_gp_row_rc[idx];
            name_rc.right = first.left - S(6);
            a52session_gamepad_name(g_session, idx, name, sizeof name);
            if (effective >= 0) snprintf(line, sizeof line, "%s  (controller %d)", name, effective + 1);
            else snprintf(line, sizeof line, "%s  (no controller)", name);
            SelectObject(hdc, g_font);
            SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
            text_line(hdc, line, &name_rc, DT_LEFT | DT_END_ELLIPSIS);
            for (seg = 0; seg <= g_nports; seg++) {
                static const char *const nums[A52_PORTS + 1] = { "Automatic", "1", "2", "3", "4" };
                RECT rect;
                segment_rect(&g_gp_row_rc[idx], seg, 1, &rect);
                draw_face(hdc, &rect, 0, seg == chosen, 0);
                text_line(hdc, nums[seg], &rect, DT_CENTER);
            }
        }
    }

    /* the mouse row: which controller a drag in the picture steers */
    {
        RECT label = g_mouse_rc, first;
        const int port = mouse_port();
        segment_rect(&g_mouse_rc, 1, 0, &first);
        label.right = first.left - S(6);
        SelectObject(hdc, g_font);
        SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
        text_line(hdc, "Dragging in the picture steers controller", &label, DT_LEFT);
        for (seg = 1; seg <= g_nports; seg++) {
            static const char *const nums[A52_PORTS + 1] = { "", "1", "2", "3", "4" };
            RECT rect;
            segment_rect(&g_mouse_rc, seg, 0, &rect);
            draw_face(hdc, &rect, 0, seg - 1 == port, 0);
            text_line(hdc, nums[seg], &rect, DT_CENTER);
        }
    }
}

/* What the hint line says about the part under an idle pointer. */
static void hover_text(int part, char *out, int outsz)
{
    const a52_control *ctl;
    char key[64], pad[64];
    out[0] = '\0';
    if (part < 0 || part >= g_numlayout) return;
    ctl = &g_layout[part];
    if (ctl->kind == A52_CTL_STICK) {
        snprintf(out, (size_t)outsz, "Controller %d analog stick: drag it, or turn the wheel over it", ctl->port + 1);
        return;
    }
    if (ctl->target < 0) return;
    binding_lines(ctl->target, key, sizeof key, pad, sizeof pad);
    snprintf(out, (size_t)outsz, "%s   Key: %s   Gamepad: %s", a52_target_name(ctl->target), key[0] ? key : "none",
             pad[0] ? pad : "none");
}

static void paint_strip(HDC hdc)
{
    int btn;
    char hover[200];
    SelectObject(hdc, g_font);
    for (btn = 0; btn < SB_COUNT; btn++) {
        const int armed = (btn == SB_MAP && g_map_state != -2) || (btn == SB_SHOW && g_show_bindings);
        const int inactive = btn == SB_CLEAR && g_map_state < 0;
        draw_face(hdc, &g_btn_rc[btn], 0, armed, inactive);
        text_line(hdc, strip_label(btn), &g_btn_rc[btn], DT_CENTER);
    }
    SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
    hover_text(g_map_state >= 0 ? -1 : g_hover_part, hover, sizeof hover);
    text_line(hdc, hover[0] ? hover : g_hint, &g_hint_rc, DT_LEFT | DT_END_ELLIPSIS);
}

/* The whole window: the controllers' art supersampled, their text, the strips.
 * Only what the update region needs is drawn. */
static void paint_rect(HDC hdc, const RECT *inval)
{
    RECT art;
    const int scale = 2;

    FillRect(hdc, inval, GetSysColorBrush(COLOR_BTNFACE));
    SetBkMode(hdc, TRANSPARENT);

    IntersectRect(&art, inval, &g_art_rc);
    if (!IsRectEmpty(&art)) {
        const int width = art.right - art.left, height = art.bottom - art.top;
        BITMAPINFO bmi;
        void *bits = NULL;
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp;
        memset(&bmi, 0, sizeof bmi);
        bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
        bmi.bmiHeader.biWidth = width * scale;
        bmi.bmiHeader.biHeight = -height * scale;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        bmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (bmp) {
            HBRUSH face = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));
            RECT all = { 0, 0, width * scale, height * scale };
            HGDIOBJ old = SelectObject(mem, bmp);
            FillRect(mem, &all, face);
            DeleteObject(face);
            SetBkMode(mem, TRANSPARENT);
            g_art_k = scale;
            g_art_x = art.left;
            g_art_y = art.top;
            g_art_rows = height * scale;
            art_all(mem);
            g_art_k = 1;
            SetStretchBltMode(hdc, HALFTONE);
            SetBrushOrgEx(hdc, 0, 0, NULL);
            StretchBlt(hdc, art.left, art.top, width, height, mem, 0, 0, width * scale, height * scale, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
        }
        DeleteDC(mem);
        SelectObject(hdc, g_font);
        text_parts(hdc);
    }
    paint_gamepads(hdc);
    paint_strip(hdc);
}

/* ---- pressing ------------------------------------------------------------------- */

static void invalidate_part(int part)
{
    RECT rect;
    if (!g_panel || part < 0 || part >= g_numlayout) return;
    part_rect(&g_layout[part], &rect);
    InflateRect(&rect, S(10), S(10));
    InvalidateRect(g_panel, &rect, FALSE);
}

static void invalidate_stick(int port)
{
    int idx;
    for (idx = 0; idx < g_numlayout; idx++)
        if (g_layout[idx].kind == A52_CTL_STICK && g_layout[idx].port == port) invalidate_part(idx);
}

/* Lets go of what the pointer holds in this window (only that: the main
 * window releases its own keys), including a wheel nudge. */
static void mouse_release(void)
{
    const int part = g_mouse_part;
    g_mouse_part = -1;
    if (g_nudging) {
        memset(g_nudge, 0, sizeof g_nudge);
        g_nudging = 0;
        if (g_panel) KillTimer(g_panel, IDT_NUDGE);
    }
    if (g_session) a52session_release_pointer(g_session);
    if (part >= 0 && part < g_numlayout) {
        invalidate_part(part);
        if (g_layout[part].kind == A52_CTL_STICK) invalidate_stick(g_layout[part].port);
    }
}

static void set_hint(const char *text)
{
    snprintf(g_hint, sizeof g_hint, "%s", text);
    g_hover_part = -1;
    if (g_panel) InvalidateRect(g_panel, &g_hint_rc, FALSE);
}

#define IDLE_HINT "Press a key or button to hold it; drag a stick. Map rebinds a key, a stick direction or a pad button."

static void set_map_state(int state)
{
    g_map_state = state;
    if (state != -2) {
        mouse_release();                    /* a held part must not outlive Map being armed */
        a52session_release_all(g_session);  /* Map swallows key-ups, so drop keyboard holds too */
    }
    if (state < 0) {
        if (g_panel) KillTimer(g_panel, IDT_CAPTURE);
        a52session_gamepad_capture_cancel(g_session);
    }
    if (state == -2) {
        set_hint(IDLE_HINT);
    } else if (state == -1) {
        set_hint("Click a key, a stick direction or a fire button to remap it. Esc finishes.");
    } else {
        char msg[200];
        snprintf(msg, sizeof msg, "Press a key or a gamepad button for %s. Esc cancels.", a52_target_name(state));
        set_hint(msg);
        a52session_gamepad_capture_begin(g_session);
        if (g_panel) SetTimer(g_panel, IDT_CAPTURE, 50, NULL);
    }
    memset(g_lit, 0, sizeof g_lit);
    if (g_panel) {
        InvalidateRect(g_panel, NULL, FALSE);
        refresh_held();
    }
}

/* After a rebind: what it is now, and whom it was taken from. */
static void bound_hint(int target, const char *what, const char *was)
{
    char msg[300], name[96];
    snprintf(name, sizeof name, "%s", a52_target_name(target));   /* a static buffer */
    if (was && was[0]) snprintf(msg, sizeof msg, "Bound %s to %s (was %s)", name, what, was);
    else snprintf(msg, sizeof msg, "Bound %s to %s", name, what);
    set_hint(msg);
}

/* ---- layout --------------------------------------------------------------------- */

/* The controllers, then the gamepad rows, the mouse row and the strip: the
 * window is this size always (for the console's port count). */
static void layout_static(void)
{
    const int left = S(MARGIN);
    int ypos = S(MARGIN), idx, xpos, content_w, art_w;

    g_nports = a52session_active_ports(g_session);
    if (g_nports < 1) g_nports = 1;
    if (g_nports > A52_PORTS) g_nports = A52_PORTS;
    g_numlayout = a52session_controls_layout(g_nports, g_layout, A52_CONTROLS_MAX, &g_layw, &g_layh);
    g_unit = (double)UNIT96 * g_dpi / 96.0;
    art_w = (int)ceil((double)g_layw * g_unit);
    content_w = art_w > S(MIN_CONTENT) ? art_w : S(MIN_CONTENT);
    g_total_w = 2 * left + content_w;

    g_orgx = left + (content_w - art_w) / 2.0;
    g_orgy = ypos;
    SetRect(&g_art_rc, left, ypos, left + content_w, ypos + (int)ceil((double)g_layh * g_unit) + S(2));
    ypos = g_art_rc.bottom + S(8);

    SetRect(&g_gp_head_rc, left, ypos, left + content_w, ypos + S(HEAD_H));
    ypos += S(HEAD_H);
    for (idx = 0; idx < PAD_ROWS; idx++) {
        SetRect(&g_gp_row_rc[idx], left, ypos, left + content_w, ypos + S(ROW_H));
        ypos += S(ROW_H + ROW_GAP);
    }
    SetRect(&g_gp_rc, left, g_gp_head_rc.top, left + content_w, ypos);
    SetRect(&g_mouse_rc, left, ypos + S(2), left + content_w, ypos + S(2) + S(ROW_H));
    ypos += S(ROW_H) + S(8);

    {
        static const int widths[SB_COUNT] = { 56, 56, 76, 108, 56 };
        xpos = left;
        for (idx = 0; idx < SB_COUNT; idx++) {
            if (idx == SB_HELP) xpos = left + content_w - S(widths[idx]);
            SetRect(&g_btn_rc[idx], xpos, ypos, xpos + S(widths[idx]), ypos + S(BTN_H));
            xpos += S(widths[idx]) + S(6);
        }
    }
    ypos += S(BTN_H) + S(6);
    SetRect(&g_hint_rc, left, ypos, left + content_w, ypos + S(HINT_H));
    g_total_h = ypos + S(HINT_H) + S(MARGIN);
}

static void rebuild(void)
{
    mouse_release();
    if (g_panel && GetCapture() == g_panel) ReleaseCapture();
    make_fonts();
    layout_static();
    memset(g_lit, 0, sizeof g_lit);
    if (g_panel) {
        InvalidateRect(g_panel, NULL, FALSE);
        refresh_held();
    }
}

/* ---- polling -------------------------------------------------------------------- */

/* What the console sees held, lit. */
static void refresh_held(void)
{
    int idx;
    if (!g_panel || g_map_state != -2) return;
    for (idx = 0; idx < g_numlayout && idx < A52_CONTROLS_MAX; idx++) {
        const a52_control *ctl = &g_layout[idx];
        unsigned char now;
        if (ctl->kind == A52_CTL_STICK) {
            int sx = 0, sy = 0;
            a52session_stick_get(g_session, ctl->port, &sx, &sy);
            now = (unsigned char)(sx != 0 || sy != 0);
            if (now != g_lit[idx] || sx != g_stick_shown[ctl->port][0] || sy != g_stick_shown[ctl->port][1]) {
                g_lit[idx] = now;
                g_stick_shown[ctl->port][0] = sx;
                g_stick_shown[ctl->port][1] = sy;
                invalidate_part(idx);
            }
            continue;
        }
        if (!pressable(ctl)) continue;
        now = (unsigned char)(a52session_target_held(g_session, ctl->target) != 0);
        if (now != g_lit[idx]) {
            g_lit[idx] = now;
            invalidate_part(idx);
        }
    }
}

/* Hot-plugged pads, auto-assignment changes, a changed port count. */
static void refresh_pads(void)
{
    pads_state now;
    int idx;
    memset(&now, 0, sizeof now);
    now.generation = a52session_gamepad_generation(g_session);
    now.count = a52session_gamepad_count(g_session);
    now.ports = a52session_active_ports(g_session);
    now.mouse = a52session_get_int(g_session, "mouse_port", 0);
    for (idx = 0; idx < PAD_ROWS && idx < now.count; idx++) {
        now.assign[idx] = a52session_gamepad_assignment(g_session, idx);
        now.effective[idx] = a52session_gamepad_effective_port(g_session, idx);
    }
    if (memcmp(&now, &g_pads_shown, sizeof now) != 0) {
        const int ports_changed = now.ports != g_pads_shown.ports && g_pads_shown.ports != 0;
        g_pads_shown = now;
        if (ports_changed && g_panel) {
            /* the BIOS changed from 4 ports to 2 or back: lay the controllers out again */
            RECT wnd, client;
            mouse_release();
            layout_static();
            GetWindowRect(g_panel, &wnd);
            GetClientRect(g_panel, &client);
            SetWindowPos(g_panel, NULL, 0, 0, (wnd.right - wnd.left) - client.right + g_total_w,
                         (wnd.bottom - wnd.top) - client.bottom + g_total_h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            memset(g_lit, 0, sizeof g_lit);
            InvalidateRect(g_panel, NULL, FALSE);
            return;
        }
        InvalidateRect(g_panel, &g_gp_rc, FALSE);
        InvalidateRect(g_panel, &g_mouse_rc, FALSE);
    }
}

/* ---- keys ----------------------------------------------------------------------- */

static void hide_panel(void)
{
    set_map_state(-2);
    mouse_release();
    if (GetCapture() == g_panel) ReleaseCapture();
    g_swallow_vk = 0;
    ShowWindow(g_panel, SW_HIDE);
}

/* A key while Map is armed: 1 if Map took it. Waiting for a target, a key is
 * not typing; waiting for a binding, the key is the binding (not F9, F11 or
 * F12, which are the app's). */
int a52_controllers_map_key(UINT msg, WPARAM vk, LPARAM lp)
{
    const int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;

    if (!g_panel) return 0;
    if (g_swallow_vk && vk == g_swallow_vk) {
        if (!down) g_swallow_vk = 0;
        return 1;
    }
    if (g_map_state == -2) return 0;
    if (vk == VK_F9 || vk == VK_F11 || vk == VK_F12) return 0;
    if (msg == WM_SYSKEYDOWN && vk == VK_F4) return 0;
    if (!down || (lp & (1 << 30))) return 1;
    if (vk == VK_ESCAPE) {
        set_map_state(g_map_state >= 0 ? -1 : -2);
        g_swallow_vk = vk;
        return 1;
    }
    if (g_map_state >= 0) {
        char was[128], name[48];
        const int target = g_map_state;
        const uint32_t ks = a52_keysym_from_msg(vk, lp);
        if (!ks) return 1;              /* nothing to call it: keep waiting */
        if (a52session_key_reserved(ks)) {
            set_hint("F9, F11 and F12 belong to the app and cannot be bound.");
            return 1;
        }
        was[0] = '\0';
        a52session_binding_set_key(g_session, target, ks, was, sizeof was);
        a52session_keysym_name(ks, name, sizeof name);
        g_swallow_vk = vk;
        set_map_state(-1);              /* stay armed: remapping several in a row is normal */
        bound_hint(target, name, was);
    }
    return 1;
}

/* A key with the window focused: 1 if taken. */
static int panel_key(UINT msg, WPARAM vk, LPARAM lp)
{
    const int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    uint32_t ks;
    int sa;

    if (a52_controllers_map_key(msg, vk, lp)) return 1;
    if (vk == VK_F9) {
        if (down && !(lp & (1 << 30))) hide_panel();
        return 1;
    }
    if (a52_app_key(vk)) {
        /* the main window's shortcuts, as if it had the focus */
        if (g_parent && down) SendMessageA(g_parent, msg, vk, lp);
        return 1;
    }
    ks = a52_keysym_from_msg(vk, lp);
    if (down && msg == WM_KEYDOWN && ks && (sa = a52session_key_sysaction(g_session, ks)) >= 0) {
        /* the main window runs these */
        if (!(lp & (1 << 30))) a52session_sysaction_post(g_session, sa);
        return 1;
    }
    return a52_forward_key(g_session, msg, vk, lp);
}

/* Exactly the laid-out client size, whatever the frame is at this DPI. */
static void fit_client(void)
{
    RECT wnd, client;
    GetWindowRect(g_panel, &wnd);
    GetClientRect(g_panel, &client);
    if (client.right == g_total_w && client.bottom == g_total_h) return;
    SetWindowPos(g_panel, NULL, 0, 0, (wnd.right - wnd.left) - client.right + g_total_w,
                 (wnd.bottom - wnd.top) - client.bottom + g_total_h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* The window's outer size for the laid-out client. */
static void window_size(HWND hwnd, SIZE *dims)
{
    RECT wnd, client;
    if (hwnd && GetWindowRect(hwnd, &wnd) && GetClientRect(hwnd, &client) && client.right > 0) {
        dims->cx = (wnd.right - wnd.left) - client.right + g_total_w;
        dims->cy = (wnd.bottom - wnd.top) - client.bottom + g_total_h;
    } else {
        SetRect(&wnd, 0, 0, g_total_w, g_total_h);
        AdjustWindowRectEx(&wnd, PANEL_STYLE, FALSE, PANEL_EXSTYLE);
        dims->cx = wnd.right - wnd.left;
        dims->cy = wnd.bottom - wnd.top;
    }
}

static void rescale(int dpi)
{
    g_dpi = dpi;
    rebuild();
}

/* ---- the mouse ------------------------------------------------------------------ */

/* The layout part under a client point, or -1. */
static int part_at(POINT pnt)
{
    if (!g_numlayout) return -1;
    return a52session_controls_hit(g_layout, g_numlayout, (float)((pnt.x - g_orgx) / g_unit),
                                   (float)((pnt.y - g_orgy) / g_unit));
}

/* A stick part's centre and drag radius, in pixels. */
static void stick_geometry(int part, double *ctrx, double *ctry, double *rad)
{
    double xpos, ypos, width, height;
    part_box(&g_layout[part], &xpos, &ypos, &width, &height);
    *ctrx = xpos + width / 2;
    *ctry = ypos + height / 2;
    *rad = width / 2;
}

/* Where the pointer is on a stick, as the analog deflection. */
static void stick_follow(int part, POINT pnt)
{
    double ctrx, ctry, rad;
    int sx, sy;
    stick_geometry(part, &ctrx, &ctry, &rad);
    a52session_stick_from_vector((float)(pnt.x - ctrx), (float)(pnt.y - ctry), (float)rad, &sx, &sy);
    a52session_stick_set(g_session, g_layout[part].port, sx, sy);
    invalidate_stick(g_layout[part].port);
}

static void mouse_down(HWND hwnd, POINT pnt)
{
    int part, row, seg, btn;
    SetFocus(hwnd);
    if (g_hover_part >= 0) {
        g_hover_part = -1;
        InvalidateRect(hwnd, &g_hint_rc, FALSE);
    }

    for (btn = 0; btn < SB_COUNT; btn++) {
        if (!PtInRect(&g_btn_rc[btn], pnt)) continue;
        switch (k_btn_id[btn]) {
        case IDC_CTRL_MAP: set_map_state(g_map_state == -2 ? -1 : -2); return;
        case IDC_CTRL_CLEAR:
            if (g_map_state >= 0) {
                a52session_binding_clear(g_session, g_map_state);
                set_hint("Cleared. Press a key or a gamepad button to bind it again");
                a52session_gamepad_capture_begin(g_session);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return;
        case IDC_CTRL_DEFAULTS:
            a52session_bindings_reset(g_session);
            {
                int pad, npads = a52session_gamepad_count(g_session);
                for (pad = 0; pad < npads; pad++) a52session_gamepad_assign(g_session, pad, -1);
                refresh_pads();
            }
            set_hint("Every key and button is back to its default");
            InvalidateRect(hwnd, NULL, FALSE);
            return;
        case IDC_CTRL_SHOW:
            g_show_bindings = !g_show_bindings;
            a52session_set_int(g_session, "controllers_show_bindings", g_show_bindings);
            InvalidateRect(hwnd, NULL, FALSE);
            return;
        case IDC_CTRL_HELP: a52_help_show(hwnd, "controllers"); return;
        default: return;
        }
    }
    for (row = 0; row < PAD_ROWS && row < a52session_gamepad_count(g_session); row++)
        for (seg = 0; seg <= g_nports; seg++) {
            RECT rect;
            segment_rect(&g_gp_row_rc[row], seg, 1, &rect);
            if (PtInRect(&rect, pnt)) {
                /* 0 = automatic (connection order), 1..N = that controller */
                a52session_gamepad_assign(g_session, row, seg - 1);
                refresh_pads();
                InvalidateRect(hwnd, &g_gp_rc, FALSE);
                return;
            }
        }
    for (seg = 1; seg <= g_nports; seg++) {
        RECT rect;
        segment_rect(&g_mouse_rc, seg, 0, &rect);
        if (PtInRect(&rect, pnt)) {
            a52session_set_int(g_session, "mouse_port", seg - 1);
            refresh_pads();
            InvalidateRect(hwnd, &g_mouse_rc, FALSE);
            return;
        }
    }

    part = part_at(pnt);
    if (part < 0) return;
    {
        const a52_control *ctl = &g_layout[part];
        if (g_map_state != -2) {
            /* armed: picks (or changes) the target; on a stick, the direction clicked */
            if (ctl->kind == A52_CTL_STICK) {
                double ctrx, ctry, rad;
                int target;
                stick_geometry(part, &ctrx, &ctry, &rad);
                target = a52session_stick_map_target(ctl->port, (float)(pnt.x - ctrx), (float)(pnt.y - ctry),
                                                      (float)rad);
                if (target >= 0) set_map_state(target);
            } else if (ctl->target >= 0) {
                set_map_state(ctl->target);
            }
            return;
        }
        mouse_release();
        g_mouse_part = part;
        if (ctl->kind == A52_CTL_STICK) stick_follow(part, pnt);
        else a52session_press(g_session, ctl->target, 1);
        invalidate_part(part);
        SetCapture(hwnd);
        refresh_held();
    }
}

static void mouse_move(HWND hwnd, POINT pnt)
{
    if (g_mouse_part >= 0) {
        if (g_layout[g_mouse_part].kind == A52_CTL_STICK) {
            stick_follow(g_mouse_part, pnt);        /* a disc is dragged, never "dragged off" */
            return;
        }
        /* dragged off what it holds: let go, as a button does */
        if (part_at(pnt) != g_mouse_part) {
            mouse_release();
            if (GetCapture() == hwnd) ReleaseCapture();
            refresh_held();
        }
        return;
    }
    {   /* idle: the hint line says what drives the part under the pointer */
        const int part = part_at(pnt);
        if (!g_tracking) {
            TRACKMOUSEEVENT track;
            memset(&track, 0, sizeof track);
            track.cbSize = sizeof track;
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = hwnd;
            g_tracking = TrackMouseEvent(&track) != 0;
        }
        if (part != g_hover_part) {
            g_hover_part = part;
            InvalidateRect(hwnd, &g_hint_rc, FALSE);
        }
    }
}

static void mouse_up(HWND hwnd)
{
    if (g_mouse_part >= 0) {
        mouse_release();
        refresh_held();
    }
    if (GetCapture() == hwnd) ReleaseCapture();
}

/* The wheel over a stick nudges it (up the wheel, up the stick); a moment
 * after the last notch it centres again. */
static void mouse_wheel(HWND hwnd, int delta, int horizontal, POINT screen)
{
    POINT pnt = screen;
    int part, port, *axis, value;
    if (g_map_state != -2 || g_mouse_part >= 0) return;
    ScreenToClient(hwnd, &pnt);
    part = part_at(pnt);
    if (part < 0 || g_layout[part].kind != A52_CTL_STICK) return;
    port = g_layout[part].port;
    axis = &g_nudge[port][horizontal ? 0 : 1];
    value = *axis + (horizontal ? 1 : -1) * (delta * NUDGE_STEP) / WHEEL_DELTA;
    if (value < -127) value = -127;
    if (value > 127) value = 127;
    *axis = value;
    a52session_stick_set(g_session, port, g_nudge[port][0], g_nudge[port][1]);
    g_nudging = 1;
    SetTimer(hwnd, IDT_NUDGE, NUDGE_MS, NULL);
    invalidate_stick(port);
    refresh_held();
}

static LRESULT CALLBACK panel_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC hdc = BeginPaint(hwnd, &paint);
        /* Off-screen first: the held highlights repaint at 25 Hz. */
        RECT inval = paint.rcPaint;
        const int width = inval.right - inval.left, height = inval.bottom - inval.top;
        if (width > 0 && height > 0) {
            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP bmp = CreateCompatibleBitmap(hdc, width, height);
            HGDIOBJ oldbmp = SelectObject(mem, bmp);
            HGDIOBJ oldfont = SelectObject(mem, g_font);
            SetViewportOrgEx(mem, -inval.left, -inval.top, NULL);
            paint_rect(mem, &inval);
            SetViewportOrgEx(mem, 0, 0, NULL);
            BitBlt(hdc, inval.left, inval.top, width, height, mem, 0, 0, SRCCOPY);
            SelectObject(mem, oldfont);
            SelectObject(mem, oldbmp);
            DeleteObject(bmp);
            DeleteDC(mem);
        }
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_LBUTTONDOWN: {
        POINT pnt;
        pnt.x = GET_X_LPARAM(lparam);
        pnt.y = GET_Y_LPARAM(lparam);
        mouse_down(hwnd, pnt);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pnt;
        pnt.x = GET_X_LPARAM(lparam);
        pnt.y = GET_Y_LPARAM(lparam);
        mouse_move(hwnd, pnt);
        return 0;
    }
    case WM_MOUSELEAVE:
        g_tracking = 0;
        if (g_hover_part >= 0) {
            g_hover_part = -1;
            InvalidateRect(hwnd, &g_hint_rc, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        /* released wherever the mouse ended up: capture means this arrives
         * even if the pointer left the part */
        mouse_up(hwnd);
        return 0;
    case WM_CAPTURECHANGED:
        /* lost the mouse some other way (Alt+Tab mid-press): let go */
        if ((HWND)lparam != hwnd && g_mouse_part >= 0) {
            mouse_release();
            refresh_held();
        }
        return 0;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        POINT screen;
        screen.x = GET_X_LPARAM(lparam);
        screen.y = GET_Y_LPARAM(lparam);
        mouse_wheel(hwnd, GET_WHEEL_DELTA_WPARAM(wparam), msg == WM_MOUSEHWHEEL, screen);
        return 0;
    }

    case WM_TIMER:
        if (wparam == IDT_CAPTURE) {
            int button;
            if (g_map_state < 0) {
                KillTimer(hwnd, IDT_CAPTURE);
            } else if (a52session_gamepad_capture_poll(g_session, &button)) {
                char was[128];
                const int target = g_map_state;
                was[0] = '\0';
                a52session_binding_set_button(g_session, target, button, was, sizeof was);
                set_map_state(-1);          /* stay armed: remapping several in a row is normal */
                bound_hint(target, a52_pad_button_name(button), was);
                InvalidateRect(hwnd, NULL, FALSE);
            }
        } else if (wparam == IDT_HELD) {
            refresh_held();
        } else if (wparam == IDT_PADS) {
            refresh_pads();
        } else if (wparam == IDT_NUDGE) {
            KillTimer(hwnd, IDT_NUDGE);
            if (g_nudging) {
                int port;
                g_nudging = 0;
                for (port = 0; port < A52_PORTS; port++)
                    if (g_nudge[port][0] || g_nudge[port][1]) {
                        g_nudge[port][0] = g_nudge[port][1] = 0;
                        a52session_stick_set(g_session, port, 0, 0);
                        invalidate_stick(port);
                    }
            }
        }
        return 0;

    /* WM_SYSKEY* too: a bound Alt is the console's */
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP:
        if (panel_key(msg, wparam, lparam)) return 0;
        break;
    case WM_SYSCHAR:
        /* Alt+letter: the main window's menu mnemonics, as when it has the focus */
        if (wparam != ' ' && g_parent) { SendMessageA(g_parent, msg, wparam, lparam); return 0; }
        break;
    case WM_ENTERMENULOOP:
        a52session_release_all(g_session);
        break;

    case WM_ACTIVATE:
        if (LOWORD(wparam) == WA_INACTIVE) {
            const HWND other = (HWND)lparam;
            mouse_release();                /* a52session_release_pointer: the pointer's holds go */
            if (GetCapture() == hwnd) ReleaseCapture();
            g_swallow_vk = 0;               /* its release goes elsewhere */
            /* Leaving the app: nothing typed here stays held (within it, the
             * window that gets the key-up lets go). */
            if (!other || GetWindowThreadProcessId(other, NULL) != GetCurrentThreadId())
                a52session_release_all(g_session);
            refresh_held();
        }
        break;                              /* DefWindowProc gives the window the focus */
    case WM_SHOWWINDOW:
        if (wparam) {
            refresh_held();
            SetTimer(hwnd, IDT_HELD, 40, NULL);
            SetTimer(hwnd, IDT_PADS, 250, NULL);
        } else {
            KillTimer(hwnd, IDT_HELD);
            KillTimer(hwnd, IDT_PADS);
            mouse_release();
        }
        break;
    case WM_GETMINMAXINFO:
        /* Fixed: the size limits say so too, so neither maximizing nor a
         * compositor (Wine passes them on) makes it any other size. */
        if (g_total_w) {
            MINMAXINFO *limits = (MINMAXINFO *)lparam;
            SIZE dims;
            window_size(hwnd, &dims);
            limits->ptMinTrackSize.x = limits->ptMaxTrackSize.x = limits->ptMaxSize.x = dims.cx;
            limits->ptMinTrackSize.y = limits->ptMaxTrackSize.y = limits->ptMaxSize.y = dims.cy;
            return 0;
        }
        break;
    case WM_DPICHANGED: {
        const RECT *suggested = (const RECT *)lparam;
        rescale(HIWORD(wparam));
        SetWindowPos(hwnd, NULL, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        fit_client();
        return 0;
    }
    case WM_CLOSE:
        /* Hide, do not destroy: a remap and the position survive closing it. */
        hide_panel();
        return 0;
    case WM_DESTROY:
        /* with the main window: the next toggle starts afresh */
        mouse_release();
        set_map_state(-2);
        g_panel = NULL;
        g_numlayout = 0;
        free_fonts();
        break;
    default:
        break;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

static int create_panel(void)
{
    static int registered;
    HINSTANCE inst = GetModuleHandleA(NULL);
    RECT want;

    g_show_bindings = a52session_get_int(g_session, "controllers_show_bindings", 0) > 0;

    if (!registered) {
        WNDCLASSEXA wcls;
        memset(&wcls, 0, sizeof wcls);
        wcls.cbSize = sizeof wcls;
        wcls.lpfnWndProc = panel_proc;      /* no CS_DBLCLKS: fast clicks stay press/release pairs */
        wcls.hInstance = inst;
        wcls.hCursor = LoadCursor(NULL, IDC_ARROW);
        wcls.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wcls.lpszClassName = PANEL_CLASS;
        registered = RegisterClassExA(&wcls) != 0;
    }
    g_dpi = a52_window_dpi(g_parent);
    make_fonts();
    layout_static();

    SetRect(&want, 0, 0, g_total_w, g_total_h);
    /* A tool window with a caption and no thick frame or maximize box: it is
     * exactly the size of its contents, and stays off the taskbar. */
    AdjustWindowRectEx(&want, PANEL_STYLE, FALSE, PANEL_EXSTYLE);
    g_panel = CreateWindowExA(PANEL_EXSTYLE, PANEL_CLASS, "Controllers (F9)", PANEL_STYLE,
                              CW_USEDEFAULT, CW_USEDEFAULT, want.right - want.left, want.bottom - want.top,
                              g_parent, NULL, inst, NULL);
    if (!g_panel) return 0;
    a52_help_set_context(g_panel, "controllers", 0);    /* F1 is the console's RESET here */
    rescale(a52_window_dpi(g_panel));    /* placed on another monitor than the main window */
    fit_client();
    memset(&g_pads_shown, 0, sizeof g_pads_shown);
    memset(g_lit, 0, sizeof g_lit);
    g_hover_part = -1;
    set_map_state(-2);
    return 1;
}

static void show_panel(void)
{
    refresh_pads();
    ShowWindow(g_panel, SW_SHOW);
    SetForegroundWindow(g_panel);
}

void a52_controllers_window_toggle(HWND parent, a52session *session)
{
    g_session = session;
    if (parent)
        g_parent = parent;
    if (!g_panel && !create_panel())
        return;
    if (IsWindowVisible(g_panel))
        hide_panel();
    else
        show_panel();
}

void a52_controllers_window_show(HWND parent, a52session *session)
{
    g_session = session;
    if (parent)
        g_parent = parent;
    if (!g_panel && !create_panel())
        return;
    if (!IsWindowVisible(g_panel))
        show_panel();
    else
        SetForegroundWindow(g_panel);
}

int a52_controllers_window_visible(void)
{
    return g_panel && IsWindowVisible(g_panel);
}

void a52_controllers_window_refresh(void)
{
    if (!g_panel)
        return;
    if (IsWindowVisible(g_panel)) {
        refresh_pads();
        refresh_held();
        InvalidateRect(g_panel, NULL, FALSE);
    }
}
