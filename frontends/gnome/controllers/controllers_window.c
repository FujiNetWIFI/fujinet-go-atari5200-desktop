/*
 * The Controllers panel -- the 5200 controller as printed: the 12-key pad
 * and START / PAUSE / RESET (drawn from the session's layout, so every
 * frontend shows the same caps), the four controller ports each with an
 * analog stick (drag it with the mouse), the four directions and the two
 * fire buttons, the gamepads and their ports, and Map mode.
 *
 * Every key and control is pressed with a raw GtkGestureClick (press AND
 * release), not GtkButton's "clicked": the console scans its keypad and
 * samples the sticks once per frame, so a press present only for the instant
 * of a click falls between frames and a polling program never sees it. A key
 * on screen is HELD for as long as the mouse button is down.
 *
 * The gesture's "cancel" is wired to the same release path as "released":
 * dragging off a key must not leave the console believing it is still down.
 * Losing the focus, hiding and rebuilding let go of whatever the mouse holds
 * here (only that: the main window releases its own keys).
 *
 * What the console sees held -- host keyboard, gamepads and this panel
 * together (a52session_keys_held, a52session_buttons_held,
 * a52session_stick_get) -- is lit (.a52-held), so the panel doubles as an
 * input tester for a new gamepad.
 *
 * Keys typed with the panel active go through the main window
 * (a52_window_key), so the console, F9 and the shortcuts behave the same
 * whichever window has the focus.
 *
 * FIXED SIZE: keys sit at KBD_UNIT pixels per key unit and every control has
 * a fixed size with labels that wrap (then ellipsize) rather than grow, so
 * neither Map mode nor a long gamepad name resizes the window, and tiling
 * compositors float it (the window is not resizable).
 *
 * SINGLETON, hidden rather than destroyed, so a remap survives closing it.
 *
 * MAP MODE: the Map button arms a two-step rebind -- click a controller
 * control or a console key to pick the target, then press a keyboard key OR
 * a gamepad button. While armed, the same gestures pick the target instead
 * of pressing, the key handler intercepts the next keystroke, and a timer
 * polls the session's gamepad capture. Rebinding steals the key from
 * whatever held it.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "controllers_window.h"

#include "../window.h"

#include <string.h>

#define KBD_UNIT 46             /* pixels per key unit */
#define KBD_GAP 3               /* between key caps */
#define CELL_W 60               /* a joystick control: three lines of Map label fit */
#define CELL_H 44
#define CELL_GAP 2
#define PORT_W (3 * CELL_W + 2 * CELL_GAP)
#define STICK_W 184
#define STICK_H 112
#define MAX_CONTROLS 128
#define MAX_PADS 8

/* ---- singleton state ------------------------------------------------------ */

static GtkWidget *g_window;
static GtkWindow *g_parent;     /* the A52Window: typed keys go through it */
static a52session *g_session;

/* Map mode: -2 idle, -1 armed and waiting for a target, >= 0 waiting for a
 * key or pad button to bind to that target. */
static int g_map_state = -2;
static GtkWidget *g_map_button;
static GtkWidget *g_map_hint;
static guint g_capture_timer;
static guint g_swallow_code;    /* the key Map took: its repeats and release too */
static guint g_held_timer;
static guint g_pad_timer;

static GtkWidget *g_sysbox;     /* power cycle / debugger / warp */
static GtkWidget *g_kbd;        /* the key caps, placed in key units */
static GtkWidget *g_ports;      /* one frame per port */
static GtkWidget *g_pad_box;
static GtkWidget *g_pad_port[MAX_PADS];   /* each pad's "drives port N" */
static int g_npad_rows;
static unsigned g_pad_generation = (unsigned)-1;

static gboolean g_built;
static int g_stick_port;        /* the port a mouse drag in the picture steers */
static GtkWidget *g_stick_area[A52_PORTS];

/* what the mouse holds here */
static int g_mouse_target = -1; /* a controller or console-key target */

typedef struct {
    GtkWidget *button;
    GtkWidget *label;
    int target;                 /* what it presses and Map rebinds, or -1 */
    gboolean lit;
} control;

static control g_controls[MAX_CONTROLS];
static int g_ncontrols;

static void refresh_labels(void);

/* Panel-only layout: key caps and controls without the theme's padding and
 * minimum sizes, so they are exactly the size given. The colours are the
 * app's (.a52-held, .a52-accent from main.c). */
static void install_css(void)
{
    static gboolean done;
    GtkCssProvider *css;
    if (done) return;
    done = TRUE;
    css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
        "button.a52-key { padding: 0 1px; min-width: 0; min-height: 0;"
        " font-size: 8pt; font-weight: bold; }\n"
        "button.a52-ctl { padding: 2px 3px; min-width: 0; min-height: 0; }\n");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}

/* ---- pressing ------------------------------------------------------------- */

static void set_lit(GtkWidget *w, gboolean on)
{
    if (on) gtk_widget_add_css_class(w, "a52-held");
    else gtk_widget_remove_css_class(w, "a52-held");
}

/* Lets go of everything the mouse holds in this panel. */
static void mouse_release(void)
{
    int port;
    if (g_mouse_target >= 0) {
        a52session_press(g_session, g_mouse_target, 0);
        g_mouse_target = -1;
    }
    for (port = 0; port < A52_PORTS; port++)
        a52session_stick_set(g_session, port, 0, 0);
}

static void control_press(control *c, int down)
{
    if (c->target < 0)
        return;
    if (c->target >= A52_TARGET_SYSACT(0)) {
        /* power cycle, debugger stop, warp: the main window runs them */
        if (down)
            a52session_sysaction_post(g_session, c->target - A52_TARGET_SYSACT(0));
        return;
    }
    if (down) {
        mouse_release();
        g_mouse_target = c->target;
        a52session_press(g_session, c->target, 1);
    } else if (g_mouse_target == c->target) {
        g_mouse_target = -1;
        a52session_press(g_session, c->target, 0);
    }
}

static void stop_capture(void)
{
    if (g_capture_timer) {
        g_source_remove(g_capture_timer);
        g_capture_timer = 0;
    }
    a52session_gamepad_capture_cancel(g_session);
}

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
        g_map_state = -1;     /* stay armed: remapping several in a row is normal */
        gtk_label_set_text(GTK_LABEL(g_map_hint), msg);
        refresh_labels();
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void set_map_state(int state)
{
    g_map_state = state;
    if (state == -2) {
        stop_capture();
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Map");
        gtk_widget_remove_css_class(g_map_button, "a52-accent");
        gtk_label_set_text(GTK_LABEL(g_map_hint),
                           "Click and hold to press. Drag in a circle to move the analog stick.");
    } else if (state == -1) {
        stop_capture();
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Done");
        gtk_widget_add_css_class(g_map_button, "a52-accent");
        gtk_label_set_text(GTK_LABEL(g_map_hint), "Click a joystick control or console key to remap");
    } else {
        char msg[128];
        mouse_release();
        g_snprintf(msg, sizeof msg, "Press a key or gamepad button for %s",
                   a52_target_name(state));
        gtk_label_set_text(GTK_LABEL(g_map_hint), msg);
        a52session_gamepad_capture_begin(g_session);
        if (!g_capture_timer)
            g_capture_timer = g_timeout_add(50, capture_tick, NULL);
    }
    refresh_labels();
}

static void on_pressed(GtkGestureClick *g, int n, double x, double y, gpointer user_data)
{
    control *c = &g_controls[GPOINTER_TO_INT(user_data)];
    (void)g; (void)n; (void)x; (void)y;

    if (g_map_state != -2) {
        /* armed: picks (or changes) the target */
        if (c->target >= 0) set_map_state(c->target);
        return;
    }
    control_press(c, 1);
    c->lit = TRUE;
    set_lit(c->button, TRUE);
}

static void on_released(GtkGestureClick *g, int n, double x, double y, gpointer user_data)
{
    (void)g; (void)n; (void)x; (void)y;
    control_press(&g_controls[GPOINTER_TO_INT(user_data)], 0);
}

static void on_cancelled(GtkGesture *g, GdkEventSequence *seq, gpointer user_data)
{
    (void)g; (void)seq;
    control_press(&g_controls[GPOINTER_TO_INT(user_data)], 0);
}

/* A fixed-size button whose label ellipsizes instead of widening it. face
 * is Pango markup. */
static GtkWidget *control_new(const char *face, int target, int w, int h,
                              const char *css_class)
{
    GtkWidget *b = gtk_button_new();
    GtkWidget *l = gtk_label_new(NULL);
    GtkGesture *g;
    int i;

    /* Up to three lines (a key and a two-word pad button wrap to fit);
     * only a name longer than that ellipsizes (its tooltip has it whole). */
    gtk_label_set_markup(GTK_LABEL(l), face);
    gtk_label_set_justify(GTK_LABEL(l), GTK_JUSTIFY_CENTER);
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(l), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_lines(GTK_LABEL(l), 3);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 1);    /* the button's size rules */
    gtk_button_set_child(GTK_BUTTON(b), l);
    gtk_widget_set_size_request(b, w, h);
    gtk_widget_set_focusable(b, FALSE);
    gtk_widget_set_focus_on_click(b, FALSE);
    gtk_widget_add_css_class(b, css_class);
    g_object_set_data_full(G_OBJECT(b), "face", g_strdup(face), g_free);

    if (g_ncontrols >= MAX_CONTROLS)
        return b;
    i = g_ncontrols++;
    g_controls[i].button = b;
    g_controls[i].label = l;
    g_controls[i].target = target;
    g_controls[i].lit = FALSE;

    g = gtk_gesture_click_new();
    g_signal_connect(g, "pressed", G_CALLBACK(on_pressed), GINT_TO_POINTER(i));
    g_signal_connect(g, "released", G_CALLBACK(on_released), GINT_TO_POINTER(i));
    g_signal_connect(g, "cancel", G_CALLBACK(on_cancelled), GINT_TO_POINTER(i));
    gtk_widget_add_controller(b, GTK_EVENT_CONTROLLER(g));
    return b;
}

/* In Map mode each remappable control shows what is bound to it, so choosing
 * what to change does not require remembering the whole table; the keys
 * that cannot be remapped are greyed. */
static void refresh_labels(void)
{
    int i;
    for (i = 0; i < g_ncontrols; i++) {
        control *c = &g_controls[i];
        gboolean key_cap = gtk_widget_has_css_class(c->button, "a52-key");
        c->lit = FALSE;
        set_lit(c->button, FALSE);
        if (g_map_state != -2 && c->target >= 0) {
            a52_binding bind = a52session_binding_get(g_session, c->target);
            char key[32], text[96], tip[160];
            a52session_keysym_name(bind.keysym, key, sizeof key);
            if (bind.button != A52_PAD_BTN_NONE) {
                /* U+2028, not \n: one paragraph, so the label's three-line
                 * cap counts both names */
                g_snprintf(text, sizeof text, "%s\xe2\x80\xa8%s", *key ? key : "\xe2\x80\x94",
                           a52_pad_button_name(bind.button));
                g_snprintf(tip, sizeof tip, "%s: %s, %s", a52_target_name(c->target),
                           *key ? key : "no key", a52_pad_button_name(bind.button));
            } else {
                g_snprintf(text, sizeof text, "%s", *key ? key : "\xe2\x80\x94");
                g_snprintf(tip, sizeof tip, "%s: %s", a52_target_name(c->target),
                           *key ? key : "no key");
            }
            gtk_label_set_text(GTK_LABEL(c->label), text);
            if (!key_cap) gtk_widget_add_css_class(c->label, "caption");
            gtk_widget_set_tooltip_text(c->button, tip);
            if (c->target == g_map_state) gtk_widget_add_css_class(c->button, "a52-accent");
            else gtk_widget_remove_css_class(c->button, "a52-accent");
            gtk_widget_set_sensitive(c->button, TRUE);
        } else {
            gtk_label_set_markup(GTK_LABEL(c->label), g_object_get_data(G_OBJECT(c->button), "face"));
            gtk_widget_remove_css_class(c->label, "caption");
            gtk_widget_set_tooltip_text(c->button, g_object_get_data(G_OBJECT(c->button), "tip"));
            gtk_widget_remove_css_class(c->button, "a52-accent");
            gtk_widget_set_sensitive(c->button, g_map_state == -2);
        }
    }
}

/* What the console sees held, lit; the sticks follow the analog values. */
static gboolean held_tick(gpointer d)
{
    unsigned held[A52_PORTS], keys;
    int i, port;
    (void)d;
    if (!g_window || !gtk_widget_get_visible(g_window))
        return G_SOURCE_CONTINUE;
    for (port = 0; port < A52_PORTS; port++) {
        held[port] = a52session_buttons_held(g_session, port);
        if (g_stick_area[port])
            gtk_widget_queue_draw(g_stick_area[port]);
    }
    keys = a52session_keys_held(g_session);
    if (g_map_state != -2)
        return G_SOURCE_CONTINUE;
    for (i = 0; i < g_ncontrols; i++) {
        control *c = &g_controls[i];
        gboolean on = FALSE;
        if (c->target >= A52_TARGET_KEY(0) && c->target < A52_TARGET_KEY(A52_KEY_COUNT))
            on = (keys >> (c->target - A52_TARGET_KEY(0))) & 1;
        else if (c->target >= 0 && c->target < A52_TARGET_KEY(0))
            on = (held[c->target / A52_ACT_PER_PORT] >> (c->target % A52_ACT_PER_PORT)) & 1;
        if (on != c->lit) {
            c->lit = on;
            set_lit(c->button, on);
        }
    }
    return G_SOURCE_CONTINUE;
}

/* ---- the keypad ------------------------------------------------------------ */

static int px(float units)
{
    return (int)(units * KBD_UNIT + 0.5f);
}

static void build_keypad(void)
{
    int n = 0, i;
    float kw = 0, kh = 0;
    const a52_keycap *caps = a52session_keyboard_layout(&n, &kw, &kh);
    float ew = 0, eh = 0;

    for (i = 0; i < n; i++) {
        const a52_keycap *k = &caps[i];
        g_autofree char *face = g_markup_escape_text(k->label, -1);
        GtkWidget *b = control_new(face, A52_TARGET_KEY(k->key),
                                   px(k->x + k->w) - px(k->x) - KBD_GAP,
                                   px(k->y + k->h) - px(k->y) - KBD_GAP, "a52-key");
        gtk_widget_set_tooltip_text(b, a52_target_name(A52_TARGET_KEY(k->key)));
        g_object_set_data(G_OBJECT(b), "tip", (gpointer)a52_target_name(A52_TARGET_KEY(k->key)));
        gtk_fixed_put(GTK_FIXED(g_kbd), b, px(k->x) + KBD_GAP / 2.0, px(k->y) + KBD_GAP / 2.0);
        if (k->x + k->w > ew) ew = k->x + k->w;
        if (k->y + k->h > eh) eh = k->y + k->h;
    }
    gtk_widget_set_size_request(g_kbd, px(ew), px(eh));
}

/* ---- the ports ----------------------------------------------------------------- */

static void stick_point(GtkWidget *area, double x, double y, int port)
{
    int w = gtk_widget_get_width(area), h = gtk_widget_get_height(area);
    double r = MIN(w, h) / 2.0 - 8.0;
    int sx = (int)CLAMP((x - w / 2.0) / r * 127.0, -128.0, 127.0);
    int sy = (int)CLAMP((y - h / 2.0) / r * 127.0, -128.0, 127.0);
    a52session_stick_set(g_session, port, sx, sy);
}

static void on_stick_begin(GtkGestureDrag *g, double x, double y, gpointer user_data)
{
    if (g_map_state != -2)
        return;
    stick_point(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g)), x, y,
                GPOINTER_TO_INT(user_data));
}

static void on_stick_update(GtkGestureDrag *g, double dx, double dy, gpointer user_data)
{
    double sx, sy;
    if (g_map_state != -2)
        return;
    gtk_gesture_drag_get_start_point(g, &sx, &sy);
    stick_point(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g)), sx + dx, sy + dy,
                GPOINTER_TO_INT(user_data));
}

static void on_stick_end(GtkGestureDrag *g, double dx, double dy, gpointer user_data)
{
    (void)g; (void)dx; (void)dy;
    a52session_stick_set(g_session, GPOINTER_TO_INT(user_data), 0, 0);
}

/* The stick's travel: a gate, the analog position as a dot (whatever drives
 * it -- this drag, the keyboard, a gamepad -- is drawn), full travel at the
 * ring. */
static void draw_stick(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer user_data)
{
    int port = GPOINTER_TO_INT(user_data), x = 0, y = 0;
    double cx = w / 2.0, cy = h / 2.0, r = MIN(w, h) / 2.0 - 8.0;
    GdkRGBA fg;
    (void)area;

    a52session_stick_get(g_session, port, &x, &y);
    gtk_widget_get_color(GTK_WIDGET(area), &fg);
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.10);
    cairo_arc(cr, cx, cy, r, 0, 2 * G_PI);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.45);
    cairo_set_line_width(cr, 1.5);
    cairo_arc(cr, cx, cy, r, 0, 2 * G_PI);
    cairo_stroke(cr);
    cairo_move_to(cr, cx - r, cy); cairo_line_to(cr, cx + r, cy);
    cairo_move_to(cr, cx, cy - r); cairo_line_to(cr, cx, cy + r);
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.2);
    cairo_stroke(cr);
    cairo_set_source_rgb(cr, ((A52SESSION_ACCENT_RGB >> 16) & 255) / 255.0,
                         ((A52SESSION_ACCENT_RGB >> 8) & 255) / 255.0,
                         (A52SESSION_ACCENT_RGB & 255) / 255.0);
    cairo_arc(cr, cx + x / 127.0 * r, cy + y / 127.0 * r, 9, 0, 2 * G_PI);
    cairo_fill(cr);
}

/* The stick's four directions around fire, and fire 2 below. */
static GtkWidget *build_joystick(int port)
{
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), CELL_GAP);
    gtk_grid_set_column_spacing(GTK_GRID(grid), CELL_GAP);
    gtk_grid_attach(GTK_GRID(grid), control_new("\xe2\x96\xb2",
        A52_TARGET_PORT(port, A52_ACT_UP), CELL_W, CELL_H, "a52-ctl"), 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), control_new("\xe2\x97\x80",
        A52_TARGET_PORT(port, A52_ACT_LEFT), CELL_W, CELL_H, "a52-ctl"), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), control_new("Fire",
        A52_TARGET_PORT(port, A52_ACT_FIRE), CELL_W, CELL_H, "a52-ctl"), 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), control_new("\xe2\x96\xb6",
        A52_TARGET_PORT(port, A52_ACT_RIGHT), CELL_W, CELL_H, "a52-ctl"), 2, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), control_new("\xe2\x96\xbc",
        A52_TARGET_PORT(port, A52_ACT_DOWN), CELL_W, CELL_H, "a52-ctl"), 1, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), control_new("Fire 2",
        A52_TARGET_PORT(port, A52_ACT_FIRE2), CELL_W, CELL_H, "a52-ctl"), 2, 2, 1, 1);
    gtk_widget_set_halign(grid, GTK_ALIGN_CENTER);
    return grid;
}

static void on_steer_toggled(GtkCheckButton *b, gpointer user_data)
{
    if (!gtk_check_button_get_active(b))
        return;
    g_stick_port = GPOINTER_TO_INT(user_data);
    if (g_parent)
        a52_window_set_stick_port(A52_WINDOW(g_parent), g_stick_port);
}

static GtkWidget *build_port(int port, GtkWidget **group_first)
{
    GtkWidget *frame = gtk_frame_new(NULL);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *title, *steer, *area, *grid;
    GtkGesture *drag;
    char text[48];

    g_snprintf(text, sizeof text, "Controller %d", port + 1);
    title = gtk_label_new(text);
    gtk_widget_add_css_class(title, "heading");
    gtk_box_append(GTK_BOX(box), title);

    steer = gtk_check_button_new_with_label("Mouse drags it");
    gtk_widget_set_tooltip_text(steer, "Dragging in the picture moves this controller's stick");
    gtk_widget_set_focus_on_click(steer, FALSE);
    if (*group_first)
        gtk_check_button_set_group(GTK_CHECK_BUTTON(steer), GTK_CHECK_BUTTON(*group_first));
    else
        *group_first = steer;
    gtk_check_button_set_active(GTK_CHECK_BUTTON(steer), port == g_stick_port);
    g_signal_connect(steer, "toggled", G_CALLBACK(on_steer_toggled), GINT_TO_POINTER(port));
    gtk_widget_set_halign(steer, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), steer);

    area = gtk_drawing_area_new();
    gtk_widget_set_size_request(area, STICK_W - 20, STICK_H);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), draw_stick, GINT_TO_POINTER(port), NULL);
    gtk_widget_set_tooltip_text(area, "Drag: the analog stick");
    drag = gtk_gesture_drag_new();
    g_signal_connect(drag, "drag-begin", G_CALLBACK(on_stick_begin), GINT_TO_POINTER(port));
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_stick_update), GINT_TO_POINTER(port));
    g_signal_connect(drag, "drag-end", G_CALLBACK(on_stick_end), GINT_TO_POINTER(port));
    gtk_widget_add_controller(area, GTK_EVENT_CONTROLLER(drag));
    g_stick_area[port] = area;
    gtk_box_append(GTK_BOX(box), area);

    grid = build_joystick(port);
    gtk_widget_set_margin_top(grid, 4);
    gtk_box_append(GTK_BOX(box), grid);

    gtk_widget_set_size_request(box, PORT_W, -1);
    gtk_widget_set_margin_top(box, 8);
    gtk_widget_set_margin_bottom(box, 10);
    gtk_widget_set_margin_start(box, 10);
    gtk_widget_set_margin_end(box, 10);
    gtk_frame_set_child(GTK_FRAME(frame), box);
    return frame;
}

/* The system actions beside the pad: mapped like any key, a click runs them. */
static GtkWidget *build_system_row(void)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    static const struct { int sa; const char *face; } acts[] = {
        { A52_SYSACT_POWER, "Power Cycle" }, { A52_SYSACT_PAUSE, "Debugger" },
        { A52_SYSACT_WARP, "Warp" },
    };
    size_t i;
    for (i = 0; i < G_N_ELEMENTS(acts); i++) {
        GtkWidget *b = control_new(acts[i].face, A52_TARGET_SYSACT(acts[i].sa),
                                   112, 34, "a52-ctl");
        gtk_widget_set_tooltip_text(b, a52_target_name(A52_TARGET_SYSACT(acts[i].sa)));
        g_object_set_data(G_OBJECT(b), "tip",
                          (gpointer)a52_target_name(A52_TARGET_SYSACT(acts[i].sa)));
        gtk_box_append(GTK_BOX(row), b);
    }
    gtk_widget_set_halign(row, GTK_ALIGN_CENTER);
    return row;
}

static void rebuild(void)
{
    GtkWidget *child, *first = NULL;
    int port;

    mouse_release();
    g_ncontrols = 0;
    memset(g_stick_area, 0, sizeof g_stick_area);
    while ((child = gtk_widget_get_first_child(g_kbd)) != NULL)
        gtk_fixed_remove(GTK_FIXED(g_kbd), child);
    while ((child = gtk_widget_get_first_child(g_ports)) != NULL)
        gtk_box_remove(GTK_BOX(g_ports), child);

    while ((child = gtk_widget_get_first_child(g_sysbox)) != NULL)
        gtk_box_remove(GTK_BOX(g_sysbox), child);
    build_keypad();
    gtk_box_append(GTK_BOX(g_sysbox), build_system_row());
    for (port = 0; port < A52_PORTS; port++)
        gtk_box_append(GTK_BOX(g_ports), build_port(port, &first));
    g_built = TRUE;
    refresh_labels();
}

/* ---- gamepads ------------------------------------------------------------- */

static void refresh_pad_ports(void)
{
    int i;
    for (i = 0; i < g_npad_rows; i++) {
        int effective = a52session_gamepad_effective_port(g_session, i);
        char text[32];
        if (effective >= 0) g_snprintf(text, sizeof text, "drives port %d", effective + 1);
        else g_snprintf(text, sizeof text, "drives nothing");
        if (strcmp(gtk_label_get_text(GTK_LABEL(g_pad_port[i])), text) != 0)
            gtk_label_set_text(GTK_LABEL(g_pad_port[i]), text);
    }
}

static void on_assign_changed(GObject *o, GParamSpec *ps, gpointer user_data)
{
    int idx = GPOINTER_TO_INT(user_data);
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(o));
    (void)ps;
    /* 0 = automatic (connection order), 1-4 = that port */
    a52session_gamepad_assign(g_session, idx, sel == 0 ? -1 : (int)sel - 1);
    refresh_pad_ports();
}

/* Rebuilt whenever a pad comes or goes (the session's generation counter):
 * one row per pad, its name, the port it drives and the port it is set to. */
static void refresh_pads(void)
{
    static const char *const choices[] = {
        "Automatic", "Port 1", "Port 2", "Port 3", "Port 4", NULL
    };
    GtkWidget *child;
    int n, i;

    while ((child = gtk_widget_get_first_child(g_pad_box)) != NULL)
        gtk_box_remove(GTK_BOX(g_pad_box), child);
    g_npad_rows = 0;

    n = a52session_gamepad_count(g_session);
    if (n == 0) {
        GtkWidget *none = gtk_label_new("No gamepads connected");
        gtk_widget_add_css_class(none, "dim-label");
        gtk_box_append(GTK_BOX(g_pad_box), none);
        return;
    }
    for (i = 0; i < n; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *name, *dd, *eff;
        char text[128];
        int assign = a52session_gamepad_assignment(g_session, i);

        a52session_gamepad_name(g_session, i, text, sizeof text);
        name = gtk_label_new(text);
        gtk_widget_set_hexpand(name, TRUE);
        gtk_label_set_xalign(GTK_LABEL(name), 0);
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(name), 1);
        gtk_widget_set_tooltip_text(name, text);
        dd = gtk_drop_down_new_from_strings(choices);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(dd),
                                   assign < 0 || assign >= A52_PORTS ? 0 : (guint)assign + 1);
        gtk_widget_set_focus_on_click(dd, FALSE);
        g_signal_connect(dd, "notify::selected", G_CALLBACK(on_assign_changed), GINT_TO_POINTER(i));
        eff = gtk_label_new("");
        gtk_widget_add_css_class(eff, "dim-label");
        gtk_box_append(GTK_BOX(row), name);
        gtk_box_append(GTK_BOX(row), eff);
        gtk_box_append(GTK_BOX(row), dd);
        gtk_box_append(GTK_BOX(g_pad_box), row);
        if (g_npad_rows < MAX_PADS)
            g_pad_port[g_npad_rows++] = eff;
    }
    refresh_pad_ports();
}

/* Hot-plugged pads and auto-assignment changes. */
static gboolean pad_tick(gpointer d)
{
    unsigned gen;
    (void)d;
    if (!g_window || !gtk_widget_get_visible(g_window)) return G_SOURCE_CONTINUE;
    gen = a52session_gamepad_generation(g_session);
    if (gen != g_pad_generation) {
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
    /* an open drop-down (a gamepad's port) has the arrows and Return */
    GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(g_window));
    if (focus && gtk_widget_get_ancestor(focus, GTK_TYPE_POPOVER))
        return FALSE;

    if (g_swallow_code && code == g_swallow_code) {
        if (!down) g_swallow_code = 0;
        return TRUE;
    }
    if (down && g_map_state >= 0) {
        guint32 keysym = code >= 8 ? a52session_keysym_from_evdev(code - 8) : 0;
        char stolen[128], msg[200], name[32];
        if (!keysym) keysym = keyval;
        a52session_binding_set_key(g_session, g_map_state, keysym, stolen, sizeof stolen);
        a52session_keysym_name(keysym, name, sizeof name);
        g_swallow_code = code;
        set_map_state(-1);   /* stay armed: remapping several in a row is normal */
        if (stolen[0])
            g_snprintf(msg, sizeof msg, "Bound %s (was %s)", name, stolen);
        else
            g_snprintf(msg, sizeof msg, "Bound %s", name);
        gtk_label_set_text(GTK_LABEL(g_map_hint), msg);
        return TRUE;
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

/* Only what this panel's mouse holds: the main window releases its own. */
static void on_focus_leave(GtkEventControllerFocus *c, gpointer d)
{
    (void)c; (void)d;
    mouse_release();
    g_swallow_code = 0;     /* its release went elsewhere */
}

/* ---- the window ----------------------------------------------------------- */

static void on_map_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    set_map_state(g_map_state == -2 ? -1 : -2);
}

static void on_defaults_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    a52session_bindings_reset(g_session);
    refresh_labels();
}

static void hide_window(void)
{
    set_map_state(-2);
    mouse_release();
    g_swallow_code = 0;
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
static void on_map(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    if (!g_held_timer) g_held_timer = g_timeout_add(33, held_tick, NULL);
    if (!g_pad_timer) g_pad_timer = g_timeout_add(250, pad_tick, NULL);
}

static void on_unmap(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
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
    g_capture_timer = g_held_timer = g_pad_timer = 0;
    g_window = NULL;
    g_ncontrols = 0;
    g_npad_rows = 0;
    memset(g_stick_area, 0, sizeof g_stick_area);
    g_built = FALSE;
    g_map_state = -2;
}

static void build_window(GtkWindow *parent)
{
    GtkWidget *root, *maprow, *toolbar, *header, *defaults, *padtitle;
    GtkEventController *keys, *focus;

    install_css();
    g_window = adw_window_new();
    gtk_window_set_title(GTK_WINDOW(g_window), "Controllers");
    if (parent) {
        gtk_window_set_transient_for(GTK_WINDOW(g_window), parent);
        /* Same app-id as the main window, so the shell groups and icons it. */
        gtk_window_set_application(GTK_WINDOW(g_window), gtk_window_get_application(parent));
    }
    gtk_window_set_destroy_with_parent(GTK_WINDOW(g_window), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(g_window), FALSE);
    g_signal_connect(g_window, "close-request", G_CALLBACK(on_close), NULL);
    g_signal_connect(g_window, "destroy", G_CALLBACK(on_destroy), NULL);

    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(root, 12);
    gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);

    g_kbd = gtk_fixed_new();
    gtk_widget_set_halign(g_kbd, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(root), g_kbd);
    g_sysbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(root), g_sysbox);

    g_ports = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_set_homogeneous(GTK_BOX(g_ports), TRUE);
    gtk_widget_set_halign(g_ports, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(root), g_ports);

    padtitle = gtk_label_new("Gamepads");
    gtk_widget_add_css_class(padtitle, "heading");
    gtk_box_append(GTK_BOX(root), padtitle);
    g_pad_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_append(GTK_BOX(root), g_pad_box);

    maprow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    g_map_button = gtk_button_new_with_label("Map");
    gtk_widget_set_size_request(g_map_button, 72, -1);
    gtk_widget_set_focus_on_click(g_map_button, FALSE);
    g_signal_connect(g_map_button, "clicked", G_CALLBACK(on_map_clicked), NULL);
    defaults = gtk_button_new_with_label("Defaults");
    gtk_widget_set_focus_on_click(defaults, FALSE);
    g_signal_connect(defaults, "clicked", G_CALLBACK(on_defaults_clicked), NULL);
    g_map_hint = gtk_label_new("");
    gtk_widget_add_css_class(g_map_hint, "dim-label");
    gtk_widget_set_hexpand(g_map_hint, TRUE);
    gtk_label_set_xalign(GTK_LABEL(g_map_hint), 0);
    gtk_label_set_ellipsize(GTK_LABEL(g_map_hint), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(g_map_hint), 1);
    gtk_box_append(GTK_BOX(maprow), g_map_button);
    gtk_box_append(GTK_BOX(maprow), defaults);
    gtk_box_append(GTK_BOX(maprow), g_map_hint);
    gtk_box_append(GTK_BOX(root), maprow);

    header = adw_header_bar_new();
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

    g_ncontrols = 0;
    g_built = FALSE;
    set_map_state(-2);
    rebuild();
    g_signal_connect(g_window, "map", G_CALLBACK(on_map), NULL);
    g_signal_connect(g_window, "unmap", G_CALLBACK(on_unmap), NULL);
}

static void show_window(void)
{
    if (!g_built)
        rebuild();
    g_pad_generation = a52session_gamepad_generation(g_session);
    refresh_pads();
    gtk_window_present(GTK_WINDOW(g_window));
}

void a52_controllers_toggle(GtkWindow *parent, a52session *session)
{
    g_session = session;
    if (parent)
        g_parent = parent;
    if (!g_window)
        build_window(g_parent);

    if (gtk_widget_get_visible(g_window))
        hide_window();
    else
        show_window();
}

gboolean a52_controllers_visible(void)
{
    return g_window && gtk_widget_get_visible(g_window);
}

void a52_controllers_refresh(void)
{
    if (!g_window)
        return;
    if (gtk_widget_get_visible(g_window))
        rebuild();
    else
        g_built = FALSE;        /* rebuilt when next shown */
}
