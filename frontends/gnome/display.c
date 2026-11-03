/*
 * display.c -- see display.h.
 *
 * Frames are pulled by serial, not pushed: copy_frame does nothing when the
 * console has not drawn anything new (an identical frame is not published), so
 * a tick between changes costs a mutex and no memcpy. The tick also tells the
 * session when the compositor shows a frame, which the emulator phase-locks
 * to. A frame is copied once, straight into a pooled buffer the texture
 * borrows (GSK reuses its GPU texture while the size holds).
 *
 * The frame's size changes with the visible area; TV aspect gives the 5200's
 * pixels their shape on a TV (NTSC 6:7, PAL a little wide). Dragging with the
 * primary button in the picture moves the analog stick of the controller
 * port the Controllers window selected.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "display.h"

#define FB_PIXELS ((gsize)A52SESSION_FB_MAX_WIDTH * A52SESSION_FB_MAX_HEIGHT)

/* ---- frame buffer pool ----------------------------------------------------
 * The session copies a frame straight into a pooled buffer (sized for the
 * largest frame) and the texture borrows it: no allocation in steady state.
 * The buffer comes back to the pool when GSK drops the texture's bytes,
 * possibly after the widget is gone, so the pool is reference counted and
 * each outstanding buffer holds a reference. */
typedef struct {
    gatomicrefcount refs;
    GMutex lock;
    GPtrArray *spare;      /* idle guint32 buffers, FB_PIXELS each */
} FramePool;

typedef struct {
    FramePool *pool;
    guint32 *px;
} FrameLoan;

#define POOL_KEEP 4

static FramePool *pool_new(void)
{
    FramePool *p = g_new0(FramePool, 1);
    g_atomic_ref_count_init(&p->refs);
    g_mutex_init(&p->lock);
    p->spare = g_ptr_array_new_with_free_func(g_free);
    return p;
}

static void pool_unref(FramePool *p)
{
    if (!g_atomic_ref_count_dec(&p->refs))
        return;
    g_ptr_array_free(p->spare, TRUE);
    g_mutex_clear(&p->lock);
    g_free(p);
}

static guint32 *pool_take(FramePool *p)
{
    guint32 *px = NULL;

    g_mutex_lock(&p->lock);
    if (p->spare->len > 0)
        px = g_ptr_array_steal_index(p->spare, p->spare->len - 1);
    g_mutex_unlock(&p->lock);
    return px ? px : g_malloc(FB_PIXELS * sizeof(guint32));
}

static void pool_give(FramePool *p, guint32 *px)
{
    g_mutex_lock(&p->lock);
    if (p->spare->len < POOL_KEEP) {
        g_ptr_array_add(p->spare, px);
        px = NULL;
    }
    g_mutex_unlock(&p->lock);
    g_free(px);
}

static void loan_release(gpointer data)
{
    FrameLoan *loan = data;

    pool_give(loan->pool, loan->px);
    pool_unref(loan->pool);
    g_free(loan);
}

struct _A52Display {
    GtkWidget parent_instance;

    a52session *session;
    FramePool *pool;
    guint32 *next;          /* a pooled buffer the next frame is copied into */
    GdkTexture *texture;
    int fw, fh, par;        /* the frame's size and pixel aspect */
    guint64 serial;
    guint tick_id;
    gulong suspended_id;    /* notify::suspended on the toplevel, while mapped */
    GtkWindow *toplevel;
    gboolean tv_aspect;
    gboolean smooth;
    graphene_rect_t shown;  /* where the picture is, for the pointer */
    double press_x, press_y;
    gboolean dragging;      /* the primary button is down: the stick follows */
    int stick_port;
};

G_DEFINE_FINAL_TYPE(A52Display, a52_display, GTK_TYPE_WIDGET)

/* Wrap px (w x h, XRGB8888 = GDK_MEMORY_B8G8R8X8 on a little-endian host; the
 * X byte is ignored, so no alpha pass) in the texture. When the size is
 * unchanged the previous texture goes in as the update source so GSK can
 * reuse its GPU texture; a new size starts afresh. Takes ownership of px. */
static void publish_texture(A52Display *self, guint32 *px, int w, int h)
{
    GdkMemoryTextureBuilder *b = gdk_memory_texture_builder_new();
    FrameLoan *loan = g_new(FrameLoan, 1);
    GBytes *bytes;

    g_atomic_ref_count_inc(&self->pool->refs);
    loan->pool = self->pool;
    loan->px = px;
    bytes = g_bytes_new_with_free_func(px, (gsize)w * h * sizeof(guint32), loan_release, loan);
    gdk_memory_texture_builder_set_bytes(b, bytes);
    gdk_memory_texture_builder_set_stride(b, (gsize)w * 4);
    gdk_memory_texture_builder_set_width(b, w);
    gdk_memory_texture_builder_set_height(b, h);
    gdk_memory_texture_builder_set_format(b, GDK_MEMORY_B8G8R8X8);
    if (self->texture && w == self->fw && h == self->fh) {
        cairo_rectangle_int_t all = { 0, 0, w, h };
        cairo_region_t *region = cairo_region_create_rectangle(&all);

        gdk_memory_texture_builder_set_update_texture(b, self->texture);
        gdk_memory_texture_builder_set_update_region(b, region);
        cairo_region_destroy(region);
    }
    g_clear_object(&self->texture);
    self->texture = gdk_memory_texture_builder_build(b);
    g_bytes_unref(bytes);
    g_object_unref(b);
}

static gboolean on_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer user_data)
{
    A52Display *self = A52_DISPLAY(widget);
    int w, h, par;
    (void)user_data;

    a52session_notify_vsync(self->session, gdk_frame_clock_get_frame_time(clock) * 1000);
    if (!self->next)
        self->next = pool_take(self->pool);
    if (a52session_copy_frame(self->session, self->next, &w, &h, &par, &self->serial) &&
        w > 0 && h > 0) {
        guint32 *px = self->next;

        self->next = NULL;
        publish_texture(self, px, w, h);
        self->fw = w;
        self->fh = h;
        self->par = par > 0 ? par : 1000;
        gtk_widget_queue_draw(widget);
    }
    return G_SOURCE_CONTINUE;
}

/* The tick runs only while somebody can see the result. A hidden or
 * suspended window gets no frame callbacks from the compositor anyway, but a
 * minimised one on some stacks keeps ticking, and every tick is a wakeup.
 * The emulator does not care: with no ticks the vsync stream goes stale and
 * the session paces on the wall clock. The first tick after the window comes
 * back copies whatever frame is newest. */
static void update_tick(A52Display *self)
{
    gboolean want = gtk_widget_get_mapped(GTK_WIDGET(self)) &&
                    !(self->toplevel && gtk_window_is_suspended(self->toplevel));

    if (want && !self->tick_id)
        self->tick_id = gtk_widget_add_tick_callback(GTK_WIDGET(self), on_tick, NULL, NULL);
    else if (!want && self->tick_id) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(self), self->tick_id);
        self->tick_id = 0;
    }
}

static void on_suspended(GObject *obj, GParamSpec *pspec, gpointer user_data)
{
    (void)obj;
    (void)pspec;
    update_tick(A52_DISPLAY(user_data));
}

static void a52_display_map(GtkWidget *widget)
{
    A52Display *self = A52_DISPLAY(widget);
    GtkRoot *root;

    GTK_WIDGET_CLASS(a52_display_parent_class)->map(widget);
    root = gtk_widget_get_root(widget);
    if (GTK_IS_WINDOW(root)) {
        self->toplevel = GTK_WINDOW(root);
        self->suspended_id = g_signal_connect(self->toplevel, "notify::suspended",
                                              G_CALLBACK(on_suspended), self);
    }
    update_tick(self);
}

static void a52_display_unmap(GtkWidget *widget)
{
    A52Display *self = A52_DISPLAY(widget);

    if (self->toplevel && self->suspended_id)
        g_signal_handler_disconnect(self->toplevel, self->suspended_id);
    self->suspended_id = 0;
    self->toplevel = NULL;
    GTK_WIDGET_CLASS(a52_display_parent_class)->unmap(widget);
    update_tick(self);
}

/* The picture's width over its height, as shown. */
static double picture_aspect(A52Display *self)
{
    double w = self->fw * self->par / 1000.0, h = self->fh;
    if (!self->tv_aspect)
        return w / h;
    /* an Atari pixel on an NTSC TV is 6/7 as wide as tall; on PAL, 1.03 */
    return w * (a52session_refresh_rate(self->session) > 55000 ? 6.0 / 7.0 : 1.03) / h;
}

static void a52_display_snapshot(GtkWidget *widget, GtkSnapshot *snapshot)
{
    A52Display *self = A52_DISPLAY(widget);
    int w = gtk_widget_get_width(widget);
    int h = gtk_widget_get_height(widget);
    double want, sw, sh;

    gtk_snapshot_append_color(snapshot, &(GdkRGBA){ 0, 0, 0, 1 },
                              &GRAPHENE_RECT_INIT(0, 0, w, h));
    if (!self->texture || w <= 0 || h <= 0)
        return;

    want = picture_aspect(self);
    if ((double)w / (double)h > want) {
        sh = h;
        sw = sh * want;
    } else {
        sw = w;
        sh = sw / want;
    }
    graphene_rect_init(&self->shown, (float)((w - sw) / 2.0), (float)((h - sh) / 2.0),
                       (float)sw, (float)sh);
    gtk_snapshot_append_scaled_texture(
        snapshot, self->texture,
        self->smooth ? GSK_SCALING_FILTER_LINEAR : GSK_SCALING_FILTER_NEAREST, &self->shown);
}

/* ---- the pointer: a drag is the analog stick -------------------------------- */

/* Dragging with the primary button held moves the analog stick of the
 * selected controller port: the stick follows the pointer's offset from where
 * it was pressed, full deflection at a sixth of the picture's width. */

static void stick_from_pointer(A52Display *self, double x, double y)
{
    double radius = self->shown.size.width > 0 ? self->shown.size.width / 6.0 : 60.0;
    int sx = (int)CLAMP((x - self->press_x) / radius * 127.0, -128.0, 127.0);
    int sy = (int)CLAMP((y - self->press_y) / radius * 127.0, -128.0, 127.0);
    a52session_stick_set(self->session, self->stick_port, sx, sy);
}

static void stick_release(A52Display *self)
{
    if (self->dragging) {
        self->dragging = FALSE;
        a52session_stick_set(self->session, self->stick_port, 0, 0);
    }
}

static void on_motion(GtkEventControllerMotion *c, double x, double y, gpointer user_data)
{
    A52Display *self = user_data;
    (void)c;
    if (self->dragging)
        stick_from_pointer(self, x, y);
}

static void on_leave(GtkEventControllerMotion *c, gpointer user_data)
{
    (void)c; (void)user_data;
}

static void on_pressed(GtkGestureClick *g, int n, double x, double y, gpointer user_data)
{
    A52Display *self = user_data;
    (void)n;
    gtk_widget_grab_focus(GTK_WIDGET(self));
    if (gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g)) != GDK_BUTTON_PRIMARY)
        return;
    self->dragging = TRUE;
    self->press_x = x;
    self->press_y = y;
    a52session_stick_set(self->session, self->stick_port, 0, 0);
}

static void on_released(GtkGestureClick *g, int n, double x, double y, gpointer user_data)
{
    (void)g; (void)n; (void)x; (void)y;
    stick_release(user_data);
}

void a52_display_set_stick_port(A52Display *self, int port)
{
    if (port != self->stick_port)
        stick_release(self);
    self->stick_port = CLAMP(port, 0, A52_PORTS - 1);
}

/* ---- the widget -------------------------------------------------------------- */

static void a52_display_dispose(GObject *object)
{
    A52Display *self = A52_DISPLAY(object);

    if (self->tick_id) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(self), self->tick_id);
        self->tick_id = 0;
    }
    g_clear_object(&self->texture);
    g_clear_pointer(&self->next, g_free);
    if (self->pool) {
        pool_unref(self->pool);
        self->pool = NULL;
    }

    G_OBJECT_CLASS(a52_display_parent_class)->dispose(object);
}

static void a52_display_class_init(A52DisplayClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = a52_display_dispose;
    GTK_WIDGET_CLASS(klass)->snapshot = a52_display_snapshot;
    GTK_WIDGET_CLASS(klass)->map = a52_display_map;
    GTK_WIDGET_CLASS(klass)->unmap = a52_display_unmap;
}

static void a52_display_init(A52Display *self)
{
    GtkEventController *motion;
    GtkGesture *click;

    self->pool = pool_new();
    self->par = 1000;
    self->tv_aspect = TRUE;
    self->smooth = TRUE;
    gtk_widget_set_focusable(GTK_WIDGET(self), TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(self), TRUE);

    motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_motion), self);
    g_signal_connect(motion, "leave", G_CALLBACK(on_leave), self);
    gtk_widget_add_controller(GTK_WIDGET(self), motion);
    click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
    g_signal_connect(click, "pressed", G_CALLBACK(on_pressed), self);
    g_signal_connect(click, "released", G_CALLBACK(on_released), self);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(click));
}

GtkWidget *a52_display_new(a52session *session)
{
    A52Display *self = g_object_new(A52_TYPE_DISPLAY, NULL);
    self->session = session;
    return GTK_WIDGET(self);
}

void a52_display_set_tv_aspect(A52Display *self, gboolean tv)
{
    self->tv_aspect = tv;
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

void a52_display_set_smooth(A52Display *self, gboolean smooth)
{
    self->smooth = smooth;
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

gboolean a52_display_save_png(A52Display *self, const char *path)
{
    return self->texture && gdk_texture_save_to_png(self->texture, path);
}

guint64 a52_display_frames(A52Display *self)
{
    /* machine frames, once a picture has reached the widget: a still screen
     * publishes nothing new, but the count still runs */
    return self->texture ? a52session_frame_count(self->session) : 0;
}
