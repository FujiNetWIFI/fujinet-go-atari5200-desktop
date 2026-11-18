/*
 * The Controllers window: the Atari 5200's controllers drawn with cairo from
 * the core's layout (a52session_controls_layout, hit-tested with
 * a52session_controls_hit): one CX52 per port the console has (four, or two
 * with the 2-port BIOS) side by side, from photos of the real thing
 * (DESIGN-5200.md). Each has its analog stick in its dish, the two pairs of
 * red side buttons (upper = FIRE, lower = FIRE 2), START / PAUSE / RESET and
 * the 12-key pad. atari800 scans one keypad and one FIRE 2 for all the
 * controllers, so a key held on any keypad lights on every controller, and a
 * line under them says so.
 *
 * Every press is the raw press and release of a gesture, not a button's
 * "clicked": the console scans its keypad and samples the sticks once per
 * frame, so a value present only for the instant of a click falls between two
 * frames. A part on screen is HELD for as long as the mouse button is down;
 * the stick follows the pointer for as long as it is dragged (analog: the
 * further from the dish's centre the further it tilts) and is let go at 0,0.
 * The wheel over a stick nudges it. Everything the pointer holds is let go
 * (a52session_release_pointer) on mouse-up, on the gesture's cancel, on focus
 * loss, when the window hides or maps and when Map is armed.
 *
 * SINGLETON, hidden rather than destroyed, so a remap survives closing it. Not
 * resizable: the canvas has a fixed size for the controllers the console has,
 * and wrapped or long labels carry width floors so the toplevel never grows.
 *
 * MAP MODE: Map arms a two-step rebind -- click a control (on the stick, the
 * direction under the click) to pick the target, then press a keyboard key OR
 * a gamepad button. Clear unbinds the picked target; Defaults restores the
 * table; Show bindings paints each control's binding on it. While a target is
 * picked the other controls dim to 40%.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "controllers_window.h"

#include "../window.h"
#include "../help/help_window.h"

#include <math.h>
#include <string.h>

#define SCALE   16.0        /* pixels per layout unit */
#define MARGIN  16.0
#define MIN_CANVAS_W 680    /* the 2-port picture is narrower than the buttons below it */
#define MAX_PADS 8
#define NUDGE_STEP 32       /* one wheel step */
#define NUDGE_HOLD_MS 700   /* a nudged stick centres itself after this */

#define HELD_R  (((A52SESSION_HELD_RGB >> 16) & 255) / 255.0)
#define HELD_G  (((A52SESSION_HELD_RGB >> 8) & 255) / 255.0)
#define HELD_B  ((A52SESSION_HELD_RGB & 255) / 255.0)

/* ---- singleton state ------------------------------------------------------ */

static GtkWidget *g_window;
static GtkWindow *g_parent;     /* the A52Window: typed keys go through it */
static a52session *g_session;
static GtkWidget *g_area;

static a52_control g_layout[A52_CONTROLS_MAX];
static int g_count;
static int g_nports;
static float g_lw, g_lh;
static double g_offx;           /* the picture is centred in the canvas */

/* Map mode: -2 idle, -1 armed and waiting for a target, >= 0 the picked
 * target waiting for a key or pad button. */
static int g_map_state = -2;
static GtkWidget *g_map_button, *g_clear_button, *g_defaults_button, *g_show_check;
static GtkWidget *g_map_hint, *g_mouse_dd;
static guint g_capture_timer, g_held_timer, g_pad_timer, g_nudge_timer;
static guint g_swallow_code;    /* the key Map took: its repeats and release too */
static GtkWidget *g_pad_box;
static GtkWidget *g_pad_port[MAX_PADS];
static int g_npad_rows;
static unsigned g_pad_generation = (unsigned)-1;
static int g_pad_nports = -1;   /* the port count the pad rows were built for */
static gboolean g_syncing;
static gboolean g_show_bindings;

static int g_drag_idx = -1;     /* the layout part the drag started on, or -1 */
static double g_ptr_x, g_ptr_y; /* the pointer, for the wheel */
static int g_nudge[A52_PORTS][2];
static guint64 g_last_sig;

/* ---- helpers --------------------------------------------------------------- */

static void set_hint(const char *text)
{
    gtk_label_set_text(GTK_LABEL(g_map_hint), text);
}

static void redraw(void)
{
    if (g_area)
        gtk_widget_queue_draw(g_area);
}

static void release_pointer(void)
{
    if (g_session)
        a52session_release_pointer(g_session);
    if (g_nudge_timer) {
        g_source_remove(g_nudge_timer);
        g_nudge_timer = 0;
    }
    memset(g_nudge, 0, sizeof g_nudge);
    g_drag_idx = -1;
    redraw();
}

static const a52_control *part(int idx)
{
    return idx >= 0 && idx < g_count ? &g_layout[idx] : NULL;
}

/* The Map target a click at canvas point (x, y) picks: a control's target; on
 * a stick the direction nearest the click (-1 within its dead centre). */
static int map_target_at(int idx, double x, double y)
{
    const a52_control *c = part(idx);
    double rad, cx, cy;
    if (!c)
        return -1;
    if (c->kind != A52_CTL_STICK)
        return c->target;
    rad = c->w / 2 * SCALE;
    cx = g_offx + (c->x + c->w / 2) * SCALE;
    cy = MARGIN + (c->y + c->h / 2) * SCALE;
    return a52session_stick_map_target(c->port, (float)(x - cx), (float)(y - cy), (float)rad);
}

/* ---- gamepad capture and Map state ---------------------------------------- */

static void stop_capture(void)
{
    if (g_capture_timer) {
        g_source_remove(g_capture_timer);
        g_capture_timer = 0;
    }
    a52session_gamepad_capture_cancel(g_session);
}

static void refresh_clear_button(void)
{
    gtk_widget_set_sensitive(g_clear_button, g_map_state >= 0);
}

static void set_map_state(int state);

static gboolean capture_tick(gpointer d)
{
    int button;
    (void)d;
    if (g_map_state < 0) { g_capture_timer = 0; return G_SOURCE_REMOVE; }
    if (a52session_gamepad_capture_poll(g_session, &button)) {
        char stolen[128], msg[200];
        a52session_binding_set_button(g_session, g_map_state, button, stolen, sizeof stolen);
        if (stolen[0])
            g_snprintf(msg, sizeof msg, "Bound %s (was %s)", a52_pad_button_name(button), stolen);
        else
            g_snprintf(msg, sizeof msg, "Bound %s", a52_pad_button_name(button));
        g_capture_timer = 0;
        set_map_state(-1);          /* back to armed: remapping several in a row is normal */
        set_hint(msg);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void set_map_state(int state)
{
    if (state != -2) {
        release_pointer();                       /* a part held into Map would never be released */
        a52session_release_all(g_session);       /* Map swallows key-ups: drop keyboard holds too */
    }
    g_map_state = state;
    if (state == -2) {
        stop_capture();
        g_swallow_code = 0;
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Map");
        gtk_widget_remove_css_class(g_map_button, "a52-accent");
        set_hint("Drag the stick, hold a key or a button. Scroll over a stick to nudge it.");
    } else if (state == -1) {
        stop_capture();
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Done");
        gtk_widget_add_css_class(g_map_button, "a52-accent");
        set_hint("Click a control to remap (on a stick, a direction). Esc backs out");
    } else {
        char msg[128];
        g_snprintf(msg, sizeof msg, "Press a key or gamepad button for %s (Esc cancels)", a52_target_name(state));
        set_hint(msg);
        stop_capture();
        a52session_gamepad_capture_begin(g_session);
        g_capture_timer = g_timeout_add(50, capture_tick, NULL);
    }
    refresh_clear_button();
    redraw();
}

/* ---- drawing ---------------------------------------------------------------- */

static void rgb(cairo_t *cr, unsigned hex)
{
    cairo_set_source_rgb(cr, ((hex >> 16) & 255) / 255.0, ((hex >> 8) & 255) / 255.0,
                         (hex & 255) / 255.0);
}

static void rgba(cairo_t *cr, unsigned hex, double a)
{
    cairo_set_source_rgba(cr, ((hex >> 16) & 255) / 255.0, ((hex >> 8) & 255) / 255.0,
                          (hex & 255) / 255.0, a);
}

static void rrect(cairo_t *cr, double x, double y, double w, double h, double r)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
}

/* Text centred on a point, shrunk to fit its width. */
static void text_fit(cairo_t *cr, const char *text, double cx, double cy, double maxw,
                     double size, cairo_font_slant_t slant, cairo_font_weight_t weight)
{
    cairo_text_extents_t ext;
    cairo_select_font_face(cr, "Sans", slant, weight);
    cairo_set_font_size(cr, size);
    cairo_text_extents(cr, text, &ext);
    if (ext.width > maxw && ext.width > 0) {
        cairo_set_font_size(cr, size * maxw / ext.width);
        cairo_text_extents(cr, text, &ext);
    }
    cairo_move_to(cr, cx - ext.width / 2 - ext.x_bearing, cy - ext.height / 2 - ext.y_bearing);
    cairo_show_text(cr, text);
}

static double px(double u) { return g_offx + u * SCALE; }
static double py(double u) { return MARGIN + u * SCALE; }

/* The CX52's body outline: a wedge, widest at the stick end. inset shrinks it
 * by that many pixels all round (the corners are rounded by stroking it). */
static void body_path(cairo_t *cr, const a52_control *b, double inset)
{
    const double x = px(b->x), y = py(b->y), w = b->w * SCALE, h = b->h * SCALE;
    const double bw = A52_BODY_BOTTOM_W * SCALE;
    cairo_new_path(cr);
    cairo_move_to(cr, x + inset, y + inset);
    cairo_line_to(cr, x + w - inset, y + inset);
    cairo_line_to(cr, x + w / 2 + bw / 2 - inset, y + h - inset);
    cairo_line_to(cr, x + w / 2 - bw / 2 + inset, y + h - inset);
    cairo_close_path(cr);
}

/* x of the body's left edge at layout y (pixels), and half the body's width. */
static void body_edges(const a52_control *b, double ly, double *left, double *right)
{
    const double t = CLAMP((ly - b->y) / b->h, 0.0, 1.0);
    const double wid = b->w - (b->w - A52_BODY_BOTTOM_W) * t;
    *left = px(b->x + (b->w - wid) / 2);
    *right = px(b->x + (b->w + wid) / 2);
}

static const a52_control *body_of(int port)
{
    int i;
    for (i = 0; i < g_count; i++)
        if (g_layout[i].kind == A52_CTL_BODY && g_layout[i].port == port)
            return &g_layout[i];
    return NULL;
}

static void draw_cable(cairo_t *cr, const a52_control *c)
{
    const double x = px(c->x + c->w / 2), y = py(c->y + c->h);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    rgb(cr, A52SESSION_CASE_SHADOW_RGB);
    cairo_set_line_width(cr, c->w * SCALE * 0.6);
    cairo_move_to(cr, x, y + 2);
    cairo_curve_to(cr, x, y - 6, x + 10, y - 2, x + 12, y - 8);
    cairo_stroke(cr);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
}

static void draw_body(cairo_t *cr, const a52_control *c)
{
    const double r = 0.8 * SCALE;
    cairo_pattern_t *p;
    double x = px(c->x), w = c->w * SCALE;

    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    /* a soft shadow under it */
    body_path(cr, c, r);
    cairo_translate(cr, 0, 3);
    rgba(cr, 0x000000, 0.35);
    cairo_set_line_width(cr, 2 * r);
    cairo_stroke_preserve(cr);
    cairo_fill(cr);
    cairo_translate(cr, 0, -3);
    /* the bevel's highlight, then the black plastic a hair inside it */
    body_path(cr, c, r);
    rgb(cr, A52SESSION_CASE_BEVEL_RGB);
    cairo_set_line_width(cr, 2 * r);
    cairo_stroke_preserve(cr);
    cairo_fill(cr);
    body_path(cr, c, r + 1.5);
    p = cairo_pattern_create_linear(x, 0, x + w, 0);
    cairo_pattern_add_color_stop_rgb(p, 0.0, 0.14, 0.14, 0.15);
    cairo_pattern_add_color_stop_rgb(p, 0.5, 0.12, 0.12, 0.125);
    cairo_pattern_add_color_stop_rgb(p, 1.0, 0.10, 0.10, 0.105);
    cairo_set_source(cr, p);
    cairo_set_line_width(cr, 2 * (r - 1.5));
    cairo_stroke_preserve(cr);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);
}

/* The Atari fuji: three strokes spreading from a common base. */
static void draw_fuji(cairo_t *cr, double cx, double base, double top, unsigned colour)
{
    const double d = (base - top) * 0.45;
    int i;
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    rgb(cr, colour);
    cairo_set_line_width(cr, 1.8);
    for (i = -1; i <= 1; i++) {
        cairo_move_to(cr, cx + i * d * 0.8, base);
        cairo_curve_to(cr, cx + i * d * 0.5, base - (base - top) * 0.5,
                       cx + i * d * 0.25, top + (base - top) * 0.2, cx + i * d * 0.12, top);
        cairo_stroke(cr);
    }
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
}

static void draw_band(cairo_t *cr, const a52_control *c)
{
    const a52_control *b = body_of(c->port);
    const double x = px(c->x), y = py(c->y), w = c->w * SCALE, h = c->h * SCALE;
    cairo_pattern_t *p;

    cairo_save(cr);
    if (b) {
        body_path(cr, b, 0);
        cairo_clip(cr);
    }
    p = cairo_pattern_create_linear(x, y, x, y + h);
    cairo_pattern_add_color_stop_rgb(p, 0.0, 0.78, 0.78, 0.80);
    cairo_pattern_add_color_stop_rgb(p, 0.35, 0.85, 0.85, 0.865);   /* BAND_HI */
    cairo_pattern_add_color_stop_rgb(p, 1.0, 0.557, 0.557, 0.58);   /* BAND_LO */
    cairo_set_source(cr, p);
    cairo_rectangle(cr, x, y, w, h);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
    /* brushed hairlines */
    rgba(cr, 0xFFFFFF, 0.25);
    cairo_set_line_width(cr, 0.5);
    for (double k = y + 2; k < y + h; k += 2.5) {
        cairo_move_to(cr, x, k + 0.5);
        cairo_line_to(cr, x + w, k + 0.5);
    }
    cairo_stroke(cr);
    /* the embossed fuji at the left, a highlight under a grey mark */
    draw_fuji(cr, x + 1.9 * SCALE + 0.8, y + h - 0.35 * SCALE + 0.8, y + 0.35 * SCALE + 0.8, 0xF4F4F6);
    draw_fuji(cr, x + 1.9 * SCALE, y + h - 0.35 * SCALE, y + 0.35 * SCALE, A52SESSION_BAND_TEXT_RGB);
    cairo_restore(cr);
}

static void draw_caption(cairo_t *cr, const a52_control *c, const GdkRGBA *fg)
{
    const double x = px(c->x), y = py(c->y), w = c->w * SCALE, h = c->h * SCALE;
    if (!strcmp(c->label, "ATARI 5200")) {
        cairo_pattern_t *p;
        rgb(cr, A52SESSION_BAND_TEXT_RGB);
        text_fit(cr, c->label, x + w / 2, y + h * 0.42, w, 13, CAIRO_FONT_SLANT_ITALIC,
                 CAIRO_FONT_WEIGHT_BOLD);
        /* the rainbow line under it */
        p = cairo_pattern_create_linear(x, 0, x + w, 0);
        cairo_pattern_add_color_stop_rgb(p, 0.0, ((A52SESSION_RAINBOW_ORANGE_RGB >> 16) & 255) / 255.0,
            ((A52SESSION_RAINBOW_ORANGE_RGB >> 8) & 255) / 255.0, (A52SESSION_RAINBOW_ORANGE_RGB & 255) / 255.0);
        cairo_pattern_add_color_stop_rgb(p, 0.33, ((A52SESSION_RAINBOW_YELLOW_RGB >> 16) & 255) / 255.0,
            ((A52SESSION_RAINBOW_YELLOW_RGB >> 8) & 255) / 255.0, (A52SESSION_RAINBOW_YELLOW_RGB & 255) / 255.0);
        cairo_pattern_add_color_stop_rgb(p, 0.66, ((A52SESSION_RAINBOW_BLUE_RGB >> 16) & 255) / 255.0,
            ((A52SESSION_RAINBOW_BLUE_RGB >> 8) & 255) / 255.0, (A52SESSION_RAINBOW_BLUE_RGB & 255) / 255.0);
        cairo_pattern_add_color_stop_rgb(p, 1.0, ((A52SESSION_RAINBOW_GREEN_RGB >> 16) & 255) / 255.0,
            ((A52SESSION_RAINBOW_GREEN_RGB >> 8) & 255) / 255.0, (A52SESSION_RAINBOW_GREEN_RGB & 255) / 255.0);
        cairo_set_source(cr, p);
        cairo_rectangle(cr, x + 2, y + h * 0.85, w - 4, 2);
        cairo_fill(cr);
        cairo_pattern_destroy(p);
    } else {                       /* "Controller N" under the body */
        cairo_set_source_rgba(cr, fg->red, fg->green, fg->blue, 0.85);
        text_fit(cr, c->label, x + w / 2, y + h / 2, w, 12, CAIRO_FONT_SLANT_NORMAL,
                 CAIRO_FONT_WEIGHT_BOLD);
    }
}

/* A raised key cap: gradient face, dark edge, light top edge. Held or picked
 * parts get the held ring and a 35% fill. */
static void cap(cairo_t *cr, double x, double y, double w, double h, double r,
                unsigned base, unsigned hi, int lit)
{
    cairo_pattern_t *p;
    /* its shadow */
    rgba(cr, 0x000000, 0.55);
    rrect(cr, x, y + 1.5, w, h, r);
    cairo_fill(cr);
    p = cairo_pattern_create_linear(x, y, x, y + h);
    cairo_pattern_add_color_stop_rgb(p, 0, ((hi >> 16) & 255) / 255.0, ((hi >> 8) & 255) / 255.0,
                                     (hi & 255) / 255.0);
    cairo_pattern_add_color_stop_rgb(p, 1, ((base >> 16) & 255) / 255.0, ((base >> 8) & 255) / 255.0,
                                     (base & 255) / 255.0);
    cairo_set_source(cr, p);
    rrect(cr, x, y, w, h, r);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(p);
    rgba(cr, 0xFFFFFF, 0.14);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    if (lit) {
        cairo_set_source_rgba(cr, HELD_R, HELD_G, HELD_B, 0.35);
        rrect(cr, x, y, w, h, r);
        cairo_fill(cr);
        rgb(cr, A52SESSION_HELD_RGB);
        cairo_set_line_width(cr, 2);
        rrect(cr, x + 1, y + 1, w - 2, h - 2, r);
        cairo_stroke(cr);
    }
}

/* What a control shows in Map mode and with Show bindings: its key and pad
 * button (a dash when there is none). */
static void binding_text(int target, char *key, int keysz, char *pad, int padsz)
{
    a52_binding b = a52session_binding_get(g_session, target);
    key[0] = pad[0] = 0;
    a52session_keysym_name(b.keysym, key, keysz);
    if (!key[0])
        g_strlcpy(key, "\xe2\x80\x94", keysz);
    if (b.button != A52_PAD_BTN_NONE)
        g_strlcpy(pad, a52_pad_button_name(b.button), padsz);
}

static gboolean bindings_on(void)
{
    return g_map_state != -2 || g_show_bindings;
}

static void draw_keycap(cairo_t *cr, const a52_control *c, int lit, double alpha)
{
    const double x = px(c->x), y = py(c->y), w = c->w * SCALE, h = c->h * SCALE;
    const gboolean console = c->kind == A52_CTL_CONSOLE_KEY;
    unsigned legend = A52SESSION_LEGEND_RGB;
    char key[64], pad[64];

    if (console) {
        legend = !strcmp(c->label, "START") ? A52SESSION_START_RGB
               : !strcmp(c->label, "RESET") ? A52SESSION_RESET_RGB : A52SESSION_PAUSE_RGB;
        /* the raised bezel the three keys sit in */
        rgba(cr, 0x000000, 0.35);
        rrect(cr, x - 2, y - 2, w + 4, h + 4, 4);
        cairo_fill(cr);
    }
    cairo_push_group(cr);
    cap(cr, x, y, w, h, console ? 3 : 4, A52SESSION_KEY_RGB, 0x2A2A2E, lit);
    if (bindings_on()) {
        binding_text(c->target, key, sizeof key, pad, sizeof pad);
        rgb(cr, legend);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 7.5);
        cairo_move_to(cr, x + 3, y + 8.5);
        cairo_show_text(cr, c->label);
        rgb(cr, 0xEDEDE6);
        text_fit(cr, key, x + w / 2, y + h * (pad[0] ? 0.50 : 0.62), w - 5, 10,
                 CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        if (pad[0]) {
            rgb(cr, 0xA9A9B2);
            text_fit(cr, pad, x + w / 2, y + h * 0.82, w - 5, 8, CAIRO_FONT_SLANT_NORMAL,
                     CAIRO_FONT_WEIGHT_NORMAL);
        }
    } else {
        rgb(cr, legend);
        if (console)
            text_fit(cr, c->label, x + w / 2, y + h / 2, w - 6, 10.5, CAIRO_FONT_SLANT_ITALIC,
                     CAIRO_FONT_WEIGHT_BOLD);
        else
            text_fit(cr, c->label, x + w / 2, y + h / 2 + (c->label[0] == '*' ? 5 : 0), w - 8,
                     c->label[0] == '*' ? 24 : 16, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    }
    cairo_pop_group_to_source(cr);
    cairo_paint_with_alpha(cr, alpha);
}

static void draw_fire(cairo_t *cr, const a52_control *c, int lit, double alpha)
{
    const double x = px(c->x), y = py(c->y), w = c->w * SCALE, h = c->h * SCALE;
    char key[64], pad[64];

    cairo_push_group(cr);
    cap(cr, x, y, w, h, 4, A52SESSION_FIRE_RGB, A52SESSION_FIRE_HI_RGB, lit);
    if (bindings_on()) {
        /* the binding runs up the narrow button */
        binding_text(c->target, key, sizeof key, pad, sizeof pad);
        cairo_save(cr);
        cairo_translate(cr, x + w / 2, y + h / 2);
        cairo_rotate(cr, -G_PI / 2);
        rgb(cr, 0xFFFFFF);
        text_fit(cr, key, 0, 0, h - 5, 9, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_restore(cr);
    }
    cairo_pop_group_to_source(cr);
    cairo_paint_with_alpha(cr, alpha);
}

static void draw_well(cairo_t *cr, const a52_control *c)
{
    const a52_control *b = body_of(c->port);
    double l0, r0, l1, r1;
    if (!b)
        return;
    body_edges(b, c->y, &l0, &r0);
    body_edges(b, c->y + c->h, &l1, &r1);
    rgb(cr, A52SESSION_WELL_RGB);
    cairo_move_to(cr, l0 + 0.5 * SCALE * 0.8, py(c->y));
    cairo_line_to(cr, r0 - 0.5 * SCALE * 0.8, py(c->y));
    cairo_line_to(cr, r1 - 0.5 * SCALE, py(c->y + c->h));
    cairo_line_to(cr, l1 + 0.5 * SCALE, py(c->y + c->h));
    cairo_close_path(cr);
    cairo_fill_preserve(cr);
    rgb(cr, A52SESSION_CASE_BEVEL_RGB);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
}

/* The stick in its dish, from above: the rim, the ribbed boot and the shaft's
 * cap shifted toward the tilt. Lit (the held ring) while it is deflected. */
static void draw_dish(cairo_t *cr, const a52_control *c, double alpha)
{
    const double r = c->w * SCALE / 2, cx = px(c->x) + r, cy = py(c->y) + r;
    int sx = 0, sy = 0;
    cairo_pattern_t *p;
    double ox, oy, capr;

    a52session_stick_get(g_session, c->port, &sx, &sy);
    ox = sx / 127.0 * 0.35 * r;
    oy = sy / 127.0 * 0.35 * r;
    capr = 0.28 * r;

    cairo_push_group(cr);
    /* the chamfered rim, lit from the top left */
    p = cairo_pattern_create_linear(cx - r, cy - r, cx + r, cy + r);
    cairo_pattern_add_color_stop_rgb(p, 0, 0.29, 0.29, 0.314);   /* DISH_RIM_HI */
    cairo_pattern_add_color_stop_rgb(p, 1, 0.149, 0.149, 0.165); /* DISH_RIM_LO */
    cairo_set_source(cr, p);
    cairo_arc(cr, cx, cy, r, 0, 2 * G_PI);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
    rgb(cr, A52SESSION_CASE_SHADOW_RGB);
    cairo_arc(cr, cx, cy, r * 0.86, 0, 2 * G_PI);
    cairo_fill(cr);
    /* the rubber boot and its rings */
    rgb(cr, A52SESSION_STICK_BOOT_RGB);
    cairo_arc(cr, cx, cy, r * 0.7, 0, 2 * G_PI);
    cairo_fill(cr);
    rgba(cr, 0x4A4A50, 0.55);
    cairo_set_line_width(cr, 1);
    cairo_arc(cr, cx, cy, r * 0.7, 0, 2 * G_PI);
    cairo_stroke(cr);
    rgba(cr, 0x34343A, 0.8);
    cairo_arc(cr, cx, cy, r * 0.58, 0, 2 * G_PI);
    cairo_stroke(cr);
    cairo_arc(cr, cx, cy, r * 0.46, 0, 2 * G_PI);
    cairo_stroke(cr);
    /* the shaft leaning toward the tilt, then its cap */
    rgb(cr, A52SESSION_STICK_SHAFT_RGB);
    cairo_set_line_width(cr, capr * 1.5);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_move_to(cr, cx, cy);
    cairo_line_to(cr, cx + ox, cy + oy);
    cairo_stroke(cr);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
    rgb(cr, A52SESSION_STICK_SHAFT_RGB);
    cairo_arc(cr, cx + ox, cy + oy, capr, 0, 2 * G_PI);
    cairo_fill(cr);
    rgba(cr, 0x5A5A62, 0.9);
    cairo_set_line_width(cr, 1.2);
    cairo_arc(cr, cx + ox, cy + oy, capr, 0, 2 * G_PI);
    cairo_stroke(cr);
    rgba(cr, 0x3C3C42, 0.9);
    cairo_set_line_width(cr, 0.8);
    cairo_arc(cr, cx + ox, cy + oy, capr * 0.7, 0, 2 * G_PI);
    cairo_stroke(cr);
    cairo_arc(cr, cx + ox, cy + oy, capr * 0.4, 0, 2 * G_PI);
    cairo_stroke(cr);
    rgba(cr, 0x7A7A84, 0.45);
    cairo_arc(cr, cx + ox - capr * 0.3, cy + oy - capr * 0.3, capr * 0.18, 0, 2 * G_PI);
    cairo_fill(cr);
    cairo_pop_group_to_source(cr);
    cairo_paint_with_alpha(cr, alpha);

    if (sx || sy) {
        rgb(cr, A52SESSION_HELD_RGB);
        cairo_set_line_width(cr, 2);
        cairo_arc(cr, cx, cy, r + 2, 0, 2 * G_PI);
        cairo_stroke(cr);
    }
}

/* The stick's part: its four directions picked in Map mode, and the keys bound
 * to them when bindings show. */
static void draw_stick_overlay(cairo_t *cr, const a52_control *c)
{
    static const int dirs[4] = { A52_ACT_UP, A52_ACT_RIGHT, A52_ACT_DOWN, A52_ACT_LEFT };
    static const double dx[4] = { 0, 1, 0, -1 }, dy[4] = { -1, 0, 1, 0 };
    const double r = c->w * SCALE / 2 * (3.4 / 2.8), cx = px(c->x) + c->w * SCALE / 2,
                 cy = py(c->y) + c->h * SCALE / 2;
    int i;

    for (i = 0; i < 4; i++) {
        const int target = A52_TARGET_PORT(c->port, dirs[i]);
        const double ex = cx + dx[i] * r * 0.78, ey = cy + dy[i] * r * 0.78;
        char key[64], pad[64];
        if (g_map_state == target) {
            /* the picked direction: a lit wedge of the dish */
            cairo_set_source_rgba(cr, HELD_R, HELD_G, HELD_B, 0.4);
            cairo_move_to(cr, cx, cy);
            cairo_arc(cr, cx, cy, r * 0.97, atan2(dy[i], dx[i]) - G_PI / 4, atan2(dy[i], dx[i]) + G_PI / 4);
            cairo_close_path(cr);
            cairo_fill(cr);
        } else if (a52session_target_held(g_session, target)) {
            rgb(cr, A52SESSION_HELD_RGB);
            cairo_set_line_width(cr, 3);
            cairo_arc(cr, cx, cy, r * 0.97, atan2(dy[i], dx[i]) - G_PI / 5, atan2(dy[i], dx[i]) + G_PI / 5);
            cairo_stroke(cr);
        }
        if (bindings_on()) {
            binding_text(target, key, sizeof key, pad, sizeof pad);
            rgba(cr, 0xEDEDE6, g_map_state >= 0 && g_map_state != target ? 0.4 : 1.0);
            text_fit(cr, key, ex, ey, i % 2 ? 24 : 38, 9, CAIRO_FONT_SLANT_NORMAL,
                     CAIRO_FONT_WEIGHT_BOLD);
        }
    }
}

static void draw_all(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer d)
{
    GdkRGBA fg;
    int i;
    (void)d; (void)height;

    gtk_widget_get_color(GTK_WIDGET(area), &fg);
    g_offx = (width - g_lw * SCALE) / 2;
    for (i = 0; i < g_count; i++) {
        const a52_control *c = &g_layout[i];
        const int held = c->target >= 0 && a52session_target_held(g_session, c->target);
        const int picked = g_map_state >= 0 && c->target == g_map_state;
        const double alpha = g_map_state >= 0 && !picked ? 0.4 : 1.0;

        switch (c->kind) {
        case A52_CTL_CABLE:   draw_cable(cr, c); break;
        case A52_CTL_BODY:    draw_body(cr, c); break;
        case A52_CTL_BAND:    draw_band(cr, c); break;
        case A52_CTL_CAPTION: draw_caption(cr, c, &fg); break;
        case A52_CTL_WELL:    draw_well(cr, c); break;
        case A52_CTL_SLOT:
            rgb(cr, A52SESSION_SLOT_RGB);
            rrect(cr, px(c->x), py(c->y), c->w * SCALE, c->h * SCALE, 1.5);
            cairo_fill(cr);
            break;
        case A52_CTL_DISH: {
            /* dimmed in Map mode unless the picked target is one of its directions */
            const int mine = g_map_state >= A52_TARGET_PORT(c->port, 0) &&
                             g_map_state < A52_TARGET_PORT(c->port, 0) + A52_ACT_PER_PORT &&
                             g_map_state % A52_ACT_PER_PORT <= A52_ACT_RIGHT;
            draw_dish(cr, c, g_map_state >= 0 && !mine ? 0.4 : 1.0);
            break;
        }
        case A52_CTL_STICK:   draw_stick_overlay(cr, c); break;
        case A52_CTL_FIRE:    draw_fire(cr, c, held || picked, alpha); break;
        case A52_CTL_CONSOLE_KEY:
        case A52_CTL_KEYCAP:  draw_keycap(cr, c, held || picked, alpha); break;
        case A52_CTL_NOTE:
            cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.7);
            text_fit(cr, c->label, px(c->x) + c->w * SCALE / 2, py(c->y) + c->h * SCALE / 2,
                     c->w * SCALE, 11, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
            break;
        }
    }
}

/* ---- sizing ---------------------------------------------------------------- */

/* The layout for the controllers the console has, and the canvas for it. */
static void relayout(void)
{
    g_nports = a52session_active_ports(g_session);
    g_count = a52session_controls_layout(g_nports, g_layout, A52_CONTROLS_MAX, &g_lw, &g_lh);
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(g_area),
                                       (int)MAX(g_lw * SCALE + 2 * MARGIN, MIN_CANVAS_W));
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(g_area), (int)(g_lh * SCALE + 2 * MARGIN));
}

static void fill_mouse_dropdown(void)
{
    GtkStringList *model = gtk_string_list_new(NULL);
    int mp = a52session_get_int(g_session, "mouse_port", 0), i;
    char text[32];
    for (i = 0; i < g_nports; i++) {
        g_snprintf(text, sizeof text, "Controller %d", i + 1);
        gtk_string_list_append(model, text);
    }
    g_syncing = TRUE;
    gtk_drop_down_set_model(GTK_DROP_DOWN(g_mouse_dd), G_LIST_MODEL(model));
    gtk_drop_down_set_selected(GTK_DROP_DOWN(g_mouse_dd), (guint)CLAMP(mp, 0, g_nports - 1));
    g_syncing = FALSE;
    g_object_unref(model);
}

/* A signature of everything that decides how the canvas looks. */
static guint64 paint_signature(void)
{
    guint64 sig = (guint64)g_nports;
    int t, port;
    for (t = 0; t < A52_TARGET_SYSACT(0) && t < 60; t++)
        if (a52session_target_held(g_session, t))
            sig |= 1ull << (t + 3);
    for (port = 0; port < g_nports; port++) {
        int x, y;
        a52session_stick_get(g_session, port, &x, &y);
        sig = sig * 1099511628211ull + (guint64)((x + 128) * 256 + (y + 128));
    }
    return sig;
}

static void refresh_pads(void);

static gboolean held_tick(gpointer d)
{
    guint64 sig;
    (void)d;
    if (!g_window || !gtk_widget_get_visible(g_window))
        return G_SOURCE_CONTINUE;
    if (a52session_active_ports(g_session) != g_nports) {
        release_pointer();
        g_drag_idx = -1;
        relayout();
        fill_mouse_dropdown();
        g_pad_nports = -1;
    }
    sig = paint_signature();
    if (sig != g_last_sig) {
        g_last_sig = sig;
        redraw();
    }
    return G_SOURCE_CONTINUE;
}

/* ---- the pointer -------------------------------------------------------------- */

static void stick_to_pointer(const a52_control *c, double x, double y)
{
    const double rad = c->w * SCALE / 2;
    const double cx = px(c->x) + rad, cy = py(c->y) + rad;
    int sx, sy;
    a52session_stick_from_vector((float)(x - cx), (float)(y - cy), (float)rad, &sx, &sy);
    a52session_stick_set(g_session, c->port, sx, sy);
}

static void on_drag_begin(GtkGestureDrag *g, double x, double y, gpointer d)
{
    const int idx = a52session_controls_hit(g_layout, g_count, (float)((x - g_offx) / SCALE),
                                            (float)((y - MARGIN) / SCALE));
    const a52_control *c = part(idx);
    (void)g; (void)d;
    if (!c)
        return;
    if (g_map_state != -2) {
        int target = map_target_at(idx, x, y);
        if (target >= 0)
            set_map_state(target);
        return;
    }
    release_pointer();
    g_drag_idx = idx;
    if (c->kind == A52_CTL_STICK)
        stick_to_pointer(c, x, y);
    else
        a52session_press(g_session, c->target, 1);
    redraw();
}

static void on_drag_update(GtkGestureDrag *g, double dx, double dy, gpointer d)
{
    const a52_control *c = part(g_drag_idx);
    double sx, sy;
    (void)d;
    if (!c || c->kind != A52_CTL_STICK || g_map_state != -2)
        return;
    gtk_gesture_drag_get_start_point(g, &sx, &sy);
    stick_to_pointer(c, sx + dx, sy + dy);
}

static void on_drag_end(GtkGestureDrag *g, double dx, double dy, gpointer d)
{
    const a52_control *c = part(g_drag_idx);
    (void)g; (void)dx; (void)dy; (void)d;
    if (c && c->kind == A52_CTL_STICK)
        a52session_stick_set(g_session, c->port, 0, 0);
    release_pointer();
}

static void on_cancelled(GtkGesture *g, GdkEventSequence *seq, gpointer d)
{
    (void)g; (void)seq; (void)d;
    release_pointer();
}

static void on_motion(GtkEventControllerMotion *c, double x, double y, gpointer d)
{
    (void)c; (void)d;
    g_ptr_x = x;
    g_ptr_y = y;
}

static gboolean nudge_done(gpointer d)
{
    int port;
    (void)d;
    for (port = 0; port < A52_PORTS; port++)
        if (g_nudge[port][0] || g_nudge[port][1]) {
            g_nudge[port][0] = g_nudge[port][1] = 0;
            a52session_stick_set(g_session, port, 0, 0);
        }
    g_nudge_timer = 0;
    return G_SOURCE_REMOVE;
}

/* The wheel over a stick nudges it (up tilts it up, sideways wheels tilt it
 * sideways); it centres itself a moment after the last step. */
static gboolean on_scroll(GtkEventControllerScroll *ctrl, double dx, double dy, gpointer d)
{
    const int idx = a52session_controls_hit(g_layout, g_count, (float)((g_ptr_x - g_offx) / SCALE),
                                            (float)((g_ptr_y - MARGIN) / SCALE));
    const a52_control *c = part(idx);
    int *n;
    (void)ctrl; (void)d;
    if (!c || c->kind != A52_CTL_STICK || g_map_state != -2 || g_drag_idx >= 0)
        return FALSE;
    n = g_nudge[c->port];
    n[0] = CLAMP(n[0] + (int)lround(dx * NUDGE_STEP), -127, 127);
    n[1] = CLAMP(n[1] + (int)lround(dy * NUDGE_STEP), -127, 127);
    a52session_stick_set(g_session, c->port, n[0], n[1]);
    if (g_nudge_timer)
        g_source_remove(g_nudge_timer);
    g_nudge_timer = g_timeout_add(NUDGE_HOLD_MS, nudge_done, NULL);
    redraw();
    return TRUE;
}

/* ---- gamepads ------------------------------------------------------------- */

static void drives_text(int idx, char *text, int size)
{
    int effective = a52session_gamepad_effective_port(g_session, idx);
    if (effective >= 0) g_snprintf(text, size, "drives controller %d", effective + 1);
    else g_snprintf(text, size, "drives nothing");
}

static void refresh_pad_ports(void)
{
    int i;
    for (i = 0; i < g_npad_rows; i++) {
        char text[40];
        drives_text(i, text, sizeof text);
        if (strcmp(gtk_label_get_text(GTK_LABEL(g_pad_port[i])), text) != 0)
            gtk_label_set_text(GTK_LABEL(g_pad_port[i]), text);
    }
}

static void on_assign_changed(GObject *o, GParamSpec *ps, gpointer user_data)
{
    int idx = GPOINTER_TO_INT(user_data);
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(o));
    (void)ps;
    if (g_syncing)
        return;
    /* 0 = automatic (connection order), 1..N = that controller */
    a52session_gamepad_assign(g_session, idx, sel == 0 ? -1 : (int)sel - 1);
    refresh_pad_ports();
}

/* Rebuilt whenever a pad comes or goes (the session's generation counter) or
 * the console's port count changes: one row per pad, its name, the controller
 * it drives and the controller it is set to. */
static void refresh_pads(void)
{
    GtkWidget *child;
    int n, i;

    while ((child = gtk_widget_get_first_child(g_pad_box)) != NULL)
        gtk_box_remove(GTK_BOX(g_pad_box), child);
    g_npad_rows = 0;
    g_pad_nports = g_nports;

    n = a52session_gamepad_count(g_session);
    if (n == 0) {
        GtkWidget *none = gtk_label_new("No gamepads connected");
        gtk_widget_add_css_class(none, "dim-label");
        gtk_label_set_xalign(GTK_LABEL(none), 0);
        gtk_box_append(GTK_BOX(g_pad_box), none);
        return;
    }
    for (i = 0; i < n; i++) {
        GtkStringList *model = gtk_string_list_new(NULL);
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *name, *dd, *eff;
        char text[128], drives[40];
        int assign = a52session_gamepad_assignment(g_session, i), k;

        gtk_string_list_append(model, "Automatic");
        for (k = 0; k < g_nports; k++) {
            char c[24];
            g_snprintf(c, sizeof c, "Controller %d", k + 1);
            gtk_string_list_append(model, c);
        }
        a52session_gamepad_name(g_session, i, text, sizeof text);
        name = gtk_label_new(text);
        gtk_widget_set_hexpand(name, TRUE);
        gtk_label_set_xalign(GTK_LABEL(name), 0);
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
        gtk_label_set_width_chars(GTK_LABEL(name), 24);
        gtk_label_set_max_width_chars(GTK_LABEL(name), 40);
        gtk_widget_set_tooltip_text(name, text);
        dd = gtk_drop_down_new(G_LIST_MODEL(model), NULL);
        g_syncing = TRUE;
        gtk_drop_down_set_selected(GTK_DROP_DOWN(dd),
                                   assign < 0 || assign >= g_nports ? 0 : (guint)assign + 1);
        g_syncing = FALSE;
        gtk_widget_set_focus_on_click(dd, FALSE);
        g_signal_connect(dd, "notify::selected", G_CALLBACK(on_assign_changed), GINT_TO_POINTER(i));
        drives_text(i, drives, sizeof drives);
        eff = gtk_label_new(drives);
        gtk_widget_add_css_class(eff, "dim-label");
        gtk_label_set_width_chars(GTK_LABEL(eff), 20);
        gtk_label_set_xalign(GTK_LABEL(eff), 0);
        gtk_box_append(GTK_BOX(row), name);
        gtk_box_append(GTK_BOX(row), eff);
        gtk_box_append(GTK_BOX(row), dd);
        gtk_box_append(GTK_BOX(g_pad_box), row);
        if (g_npad_rows < MAX_PADS)
            g_pad_port[g_npad_rows++] = eff;
    }
}

/* Hot-plugged pads and auto-assignment changes. */
static gboolean pad_tick(gpointer d)
{
    unsigned gen;
    (void)d;
    if (!g_window || !gtk_widget_get_visible(g_window)) return G_SOURCE_CONTINUE;
    gen = a52session_gamepad_generation(g_session);
    if (gen != g_pad_generation || g_pad_nports != g_nports) {
        g_pad_generation = gen;
        refresh_pads();
    } else {
        refresh_pad_ports();
    }
    return G_SOURCE_CONTINUE;
}

/* ---- keyboard ------------------------------------------------------------- */

static gboolean on_key(guint keyval, guint code, GdkModifierType st, gboolean down)
{
    /* an open drop-down (a gamepad's controller) has the arrows and Return */
    GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(g_window));
    if (focus && gtk_widget_get_ancestor(focus, GTK_TYPE_POPOVER))
        return FALSE;

    /* F9, F11, F12 and Ctrl chords are never Map's: they go where they always do */
    if (g_map_state != -2 && !(keyval == GDK_KEY_F9 || keyval == GDK_KEY_F11 ||
                               keyval == GDK_KEY_F12 || (st & GDK_CONTROL_MASK))) {
        if (g_swallow_code && code == g_swallow_code) {
            if (!down) g_swallow_code = 0;
            return TRUE;
        }
        if (!down)
            return TRUE;
        if (keyval == GDK_KEY_Escape) {
            g_swallow_code = code;
            set_map_state(g_map_state >= 0 ? -1 : -2);
            return TRUE;
        }
        if (g_map_state < 0)
            return TRUE;           /* armed, nothing picked: keys do nothing */
        {
            guint32 keysym = code >= 8 ? a52session_keysym_from_evdev(code - 8) : 0;
            char stolen[128], msg[200], name[32];
            if (!keysym) keysym = keyval;
            if (a52session_key_reserved(keysym)) {
                set_hint("F9, F11 and F12 can't be bound");
                g_swallow_code = code;
                return TRUE;
            }
            stolen[0] = '\0';
            a52session_binding_set_key(g_session, g_map_state, keysym, stolen, sizeof stolen);
            a52session_keysym_name(keysym, name, sizeof name);
            g_swallow_code = code;
            set_map_state(-1);     /* back to armed */
            if (stolen[0])
                g_snprintf(msg, sizeof msg, "Bound %s (was %s)", name, stolen);
            else
                g_snprintf(msg, sizeof msg, "Bound %s", name);
            set_hint(msg);
            return TRUE;
        }
    }
    /* Otherwise exactly what the main window does with it. */
    if (!g_parent) return FALSE;
    return a52_window_key(A52_WINDOW(g_parent), keyval, code, st, down);
}

static gboolean on_key_pressed(GtkEventControllerKey *c, guint keyval,
                               guint code, GdkModifierType st, gpointer d)
{
    (void)c; (void)d;
    return on_key(keyval, code, st, TRUE);
}

static void on_key_released(GtkEventControllerKey *c, guint keyval,
                            guint code, GdkModifierType st, gpointer d)
{
    (void)c; (void)d;
    on_key(keyval, code, st, FALSE);
}

/* Keys and mouse buttons held into a focus change must not stay down: the
 * release goes to another window. */
static void on_focus_leave(GtkEventControllerFocus *c, gpointer d)
{
    (void)c; (void)d;
    release_pointer();
    a52session_release_all(g_session);
    g_swallow_code = 0;
}

static void on_active_changed(GObject *o, GParamSpec *ps, gpointer d)
{
    (void)ps; (void)d;
    if (!gtk_window_is_active(GTK_WINDOW(o)))
        release_pointer();
}

/* ---- the window ----------------------------------------------------------- */

static void on_map_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    set_map_state(g_map_state == -2 ? -1 : -2);
}

static void on_clear_clicked(GtkButton *b, gpointer d)
{
    char msg[160];
    (void)b; (void)d;
    if (g_map_state < 0) return;
    a52session_binding_clear(g_session, g_map_state);
    g_snprintf(msg, sizeof msg, "Cleared %s", a52_target_name(g_map_state));
    set_hint(msg);
    redraw();
}

static void on_defaults_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    int i, n = a52session_gamepad_count(g_session);
    a52session_bindings_reset(g_session);
    for (i = 0; i < n; i++) a52session_gamepad_assign(g_session, i, -1);
    refresh_pads();
    set_hint("Back to the defaults");
    redraw();
}

static void on_show_toggled(GtkCheckButton *b, gpointer d)
{
    (void)d;
    g_show_bindings = gtk_check_button_get_active(b);
    a52session_set_int(g_session, "controllers_show_bindings", g_show_bindings);
    redraw();
}

static void on_mouse_changed(GObject *o, GParamSpec *ps, gpointer d)
{
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(o));
    (void)ps; (void)d;
    if (g_syncing || sel == GTK_INVALID_LIST_POSITION)
        return;
    a52session_set_int(g_session, "mouse_port", (int)sel);
    if (g_parent)
        a52_window_set_stick_port(A52_WINDOW(g_parent), (int)sel);
}

static void hide_window(void)
{
    set_map_state(-2);
    release_pointer();
    gtk_widget_set_visible(g_window, FALSE);
}

static gboolean on_close(GtkWindow *w, gpointer d)
{
    (void)w; (void)d;
    hide_window();
    return TRUE;
}

/* The refresh timers run only while the window is on screen: closing only
 * hides it, and a hidden window has nobody to update. */
static void on_window_map(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    release_pointer();
    if (!g_held_timer) g_held_timer = g_timeout_add(33, held_tick, NULL);
    if (!g_pad_timer) g_pad_timer = g_timeout_add(250, pad_tick, NULL);
}

static void on_window_unmap(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    release_pointer();
    if (g_held_timer) g_source_remove(g_held_timer);
    if (g_pad_timer) g_source_remove(g_pad_timer);
    g_held_timer = g_pad_timer = 0;
}

static void on_destroy(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    if (g_capture_timer) g_source_remove(g_capture_timer);
    if (g_held_timer) g_source_remove(g_held_timer);
    if (g_pad_timer) g_source_remove(g_pad_timer);
    if (g_nudge_timer) g_source_remove(g_nudge_timer);
    g_capture_timer = g_held_timer = g_pad_timer = g_nudge_timer = 0;
    g_window = NULL;
    g_area = NULL;
    g_npad_rows = 0;
    g_map_state = -2;
}

static GtkWidget *plain_button(const char *label, GCallback cb)
{
    GtkWidget *b = gtk_button_new_with_label(label);
    gtk_widget_set_focus_on_click(b, FALSE);
    g_signal_connect(b, "clicked", cb, NULL);
    return b;
}

static void build_window(GtkWindow *parent)
{
    GtkWidget *root, *maprow, *toolbar, *header, *padtitle, *padscroll, *mouse_label;
    GtkEventController *keys, *focus, *motion, *scroll;
    GtkGesture *drag;

    g_parent = parent;
    g_window = adw_window_new();
    gtk_window_set_title(GTK_WINDOW(g_window), "Controllers (F9)");
    if (parent) {
        gtk_window_set_transient_for(GTK_WINDOW(g_window), parent);
        /* Same app-id as the main window, so the shell groups and icons it. */
        gtk_window_set_application(GTK_WINDOW(g_window), gtk_window_get_application(parent));
    }
    gtk_window_set_destroy_with_parent(GTK_WINDOW(g_window), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(g_window), FALSE);
    g_signal_connect(g_window, "close-request", G_CALLBACK(on_close), NULL);
    g_signal_connect(g_window, "destroy", G_CALLBACK(on_destroy), NULL);
    g_signal_connect(g_window, "notify::is-active", G_CALLBACK(on_active_changed), NULL);

    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(root, 8);
    gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);

    g_area = gtk_drawing_area_new();
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(g_area), draw_all, NULL, NULL);
    gtk_widget_set_halign(g_area, GTK_ALIGN_CENTER);
    gtk_widget_set_focusable(g_area, FALSE);
    drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_begin), NULL);
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), NULL);
    g_signal_connect(drag, "drag-end", G_CALLBACK(on_drag_end), NULL);
    g_signal_connect(drag, "cancel", G_CALLBACK(on_cancelled), NULL);
    gtk_widget_add_controller(g_area, GTK_EVENT_CONTROLLER(drag));
    motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_motion), NULL);
    g_signal_connect(motion, "enter", G_CALLBACK(on_motion), NULL);
    gtk_widget_add_controller(g_area, motion);
    scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    g_signal_connect(scroll, "scroll", G_CALLBACK(on_scroll), NULL);
    gtk_widget_add_controller(g_area, scroll);
    gtk_box_append(GTK_BOX(root), g_area);
    relayout();

    maprow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    g_map_button = plain_button("Map", G_CALLBACK(on_map_clicked));
    gtk_widget_set_size_request(g_map_button, 72, -1);
    g_clear_button = plain_button("Clear", G_CALLBACK(on_clear_clicked));
    g_defaults_button = plain_button("Defaults", G_CALLBACK(on_defaults_clicked));
    g_show_check = gtk_check_button_new_with_label("Show bindings");
    g_show_bindings = a52session_get_int(g_session, "controllers_show_bindings", 0) != 0;
    gtk_check_button_set_active(GTK_CHECK_BUTTON(g_show_check), g_show_bindings);
    gtk_widget_set_focus_on_click(g_show_check, FALSE);
    g_signal_connect(g_show_check, "toggled", G_CALLBACK(on_show_toggled), NULL);
    mouse_label = gtk_label_new("Mouse in the picture drives");
    gtk_widget_set_hexpand(mouse_label, TRUE);
    gtk_widget_set_halign(mouse_label, GTK_ALIGN_END);
    gtk_widget_add_css_class(mouse_label, "dim-label");
    g_mouse_dd = gtk_drop_down_new(NULL, NULL);
    gtk_widget_set_focus_on_click(g_mouse_dd, FALSE);
    gtk_widget_set_tooltip_text(g_mouse_dd,
        "The controller whose stick a mouse drag in the main picture moves");
    fill_mouse_dropdown();
    g_signal_connect(g_mouse_dd, "notify::selected", G_CALLBACK(on_mouse_changed), NULL);
    gtk_box_append(GTK_BOX(maprow), g_map_button);
    gtk_box_append(GTK_BOX(maprow), g_clear_button);
    gtk_box_append(GTK_BOX(maprow), g_defaults_button);
    gtk_box_append(GTK_BOX(maprow), g_show_check);
    gtk_box_append(GTK_BOX(maprow), mouse_label);
    gtk_box_append(GTK_BOX(maprow), g_mouse_dd);
    gtk_box_append(GTK_BOX(root), maprow);

    g_map_hint = gtk_label_new("");
    gtk_widget_add_css_class(g_map_hint, "dim-label");
    gtk_widget_set_hexpand(g_map_hint, TRUE);
    /* a long hint ("Press a key or gamepad button for Controller 2: Fire 2")
     * must not widen the window */
    gtk_label_set_ellipsize(GTK_LABEL(g_map_hint), PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(GTK_LABEL(g_map_hint), 24);
    gtk_label_set_max_width_chars(GTK_LABEL(g_map_hint), 80);
    gtk_label_set_xalign(GTK_LABEL(g_map_hint), 0);
    gtk_box_append(GTK_BOX(root), g_map_hint);

    padtitle = gtk_label_new("Gamepads");
    gtk_widget_add_css_class(padtitle, "heading");
    gtk_label_set_xalign(GTK_LABEL(padtitle), 0);
    gtk_box_append(GTK_BOX(root), padtitle);
    g_pad_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    /* two pad rows tall whatever is plugged in (more scroll), so a pad coming
     * or going never resizes the window */
    padscroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(padscroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(padscroll), FALSE);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(padscroll), 84);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(padscroll), 84);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(padscroll), g_pad_box);
    gtk_box_append(GTK_BOX(root), padscroll);

    header = adw_header_bar_new();
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), a52_help_button("controllers"));
    a52_help_set_context(GTK_WINDOW(g_window), "controllers");
    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), root);
    adw_window_set_content(ADW_WINDOW(g_window), toolbar);

    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), NULL);
    g_signal_connect(keys, "key-released", G_CALLBACK(on_key_released), NULL);
    gtk_widget_add_controller(g_window, keys);

    focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "leave", G_CALLBACK(on_focus_leave), NULL);
    gtk_widget_add_controller(g_window, focus);

    g_signal_connect(g_window, "map", G_CALLBACK(on_window_map), NULL);
    g_signal_connect(g_window, "unmap", G_CALLBACK(on_window_unmap), NULL);
    set_map_state(-2);
}

void a52_controllers_show(GtkWindow *parent, a52session *session)
{
    g_session = session;
    if (!g_window)
        build_window(parent);
    else if (parent)
        g_parent = parent;
    if (a52session_active_ports(g_session) != g_nports) {
        relayout();
        fill_mouse_dropdown();
    }
    g_pad_generation = a52session_gamepad_generation(g_session);
    refresh_pads();
    gtk_window_present(GTK_WINDOW(g_window));
}

void a52_controllers_toggle(GtkWindow *parent, a52session *session)
{
    g_session = session;
    if (g_window && gtk_widget_get_visible(g_window))
        hide_window();
    else
        a52_controllers_show(parent, session);
}

gboolean a52_controllers_visible(void)
{
    return g_window && gtk_widget_get_visible(g_window);
}

gboolean a52_controllers_active(void)
{
    return g_window && gtk_window_is_active(GTK_WINDOW(g_window));
}

/* Bindings or settings changed outside the window: the port count may have,
 * and so may what the pad rows offer. */
void a52_controllers_refresh(void)
{
    if (!g_window)
        return;
    if (a52session_active_ports(g_session) != g_nports) {
        relayout();
        fill_mouse_dropdown();
    }
    g_pad_nports = -1;
    redraw();
}
