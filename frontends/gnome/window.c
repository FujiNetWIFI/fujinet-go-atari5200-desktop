/*
 * window.c -- the main window: the 5200's picture, the header-bar menu, the
 * keyboard, the BIOS import, the FujiNet status, and the timer that brings
 * the gamepad thread's news to the UI.
 *
 * Keys go to the console pressed and released, keyed by the physical key
 * (GDK's hardware keycode is evdev + 8): the session binds them to the
 * controller 1 stick, fire, the 12-key pad and START / PAUSE / RESET. The
 * window's own keys are F1 and F6 (Help), F9 (Controllers), F11
 * (fullscreen), F12 (debugger) and Shift+F5 (power cycle).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "window.h"

#include "controllers/controllers_window.h"
#include "debugger/dbg_window.h"
#include "display.h"
#include "fujilog.h"
#include "help/help_window.h"
#include "prefs.h"

#include <string.h>

struct _A52Window {
    AdwApplicationWindow parent_instance;

    a52session *session;
    GtkWidget *display;
    GtkWidget *toast_overlay;
    GtkWidget *status;
    GtkWidget *status_dot;
    GtkWidget *menu_button;
    char menu_sig[1024];        /* what the menus last showed */
    guint status_id;
    guint fast_id;
    unsigned pad_generation;
    unsigned fujinet_generation;
    gboolean fullscreen;
    gboolean notice_open;
    /* A52_SCREENSHOT / A52_AUTOQUIT_FRAMES */
    char *shot_path;
    guint64 shot_frames, quit_frames;
};

G_DEFINE_FINAL_TYPE(A52Window, a52_window, ADW_TYPE_APPLICATION_WINDOW)

static void rebuild_menu(A52Window *self, gboolean force);
static void update_status(A52Window *self);

void a52_window_toast(A52Window *self, const char *text)
{
    adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(self->toast_overlay), adw_toast_new(text));
}

/* ---- the accent ---------------------------------------------------------------- */

static GtkCssProvider *s_accent_css;

static void load_accent_css(gboolean dark)
{
    char buf[1024];
    /* rust fills with white text everywhere; as text colour the rust is too
     * dark on a dark theme, so there it is lightened (#C8A27A, 7:1) */
    g_snprintf(buf, sizeof buf,
        ".a52-accent, .a52-held { background: #%06x; color: #ffffff; }\n"
        ".a52-accent:hover { background: shade(#%06x, 1.15); }\n"
        ".a52-accent-text { color: %s; font-weight: bold; }\n"
        ".a52-dot { border-radius: 6px; min-width: 12px; min-height: 12px; }\n"
        ".a52-dot-on { background: #%06x; }\n"
        ".a52-dot-off { background: #808080; }\n",
        A52SESSION_ACCENT_RGB, A52SESSION_ACCENT_RGB, dark ? "#c8a27a" : "#765c3c",
        A52SESSION_ACCENT_RGB);
    gtk_css_provider_load_from_string(s_accent_css, buf);
}

static void on_dark_changed(AdwStyleManager *sm, GParamSpec *ps, gpointer ud)
{
    (void)ps; (void)ud;
    load_accent_css(adw_style_manager_get_dark(sm));
}

void a52_install_accent_css(void)
{
    AdwStyleManager *sm = adw_style_manager_get_default();
    if (s_accent_css)
        return;
    s_accent_css = gtk_css_provider_new();
    load_accent_css(adw_style_manager_get_dark(sm));
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(s_accent_css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_signal_connect(sm, "notify::dark", G_CALLBACK(on_dark_changed), NULL);
}

/* ---- small helpers ---------------------------------------------------------------- */

static void show_message(GtkWidget *over, const char *heading, const char *body)
{
    AdwDialog *dlg = adw_alert_dialog_new(heading, body);
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dlg), "ok", "OK");
    adw_dialog_present(dlg, over);
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* GMenu labels take _ as a mnemonic: a file name keeps its own. */
static char *menu_escape(const char *s)
{
    GString *g = g_string_new(NULL);
    for (; *s; s++) {
        if (*s == '_')
            g_string_append_c(g, '_');
        g_string_append_c(g, *s);
    }
    return g_string_free(g, FALSE);
}

static GtkFileDialog *file_dialog(const char *title, const char *filter_name,
                                  const char *const *exts, const char *folder)
{
    GtkFileDialog *dlg = gtk_file_dialog_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    GtkFileFilter *these = gtk_file_filter_new();
    GtkFileFilter *all = gtk_file_filter_new();

    gtk_file_filter_set_name(these, filter_name);
    if (!exts)
        gtk_file_filter_add_pattern(these, "*");
    for (int i = 0; exts && exts[i]; i++)
        gtk_file_filter_add_suffix(these, exts[i]);   /* any case */
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    g_list_store_append(filters, these);
    g_list_store_append(filters, all);
    gtk_file_dialog_set_title(dlg, title);
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    if (folder && *folder && g_file_test(folder, G_FILE_TEST_IS_DIR)) {
        g_autoptr(GFile) dir = g_file_new_for_path(folder);
        gtk_file_dialog_set_initial_folder(dlg, dir);
    }
    g_object_unref(these);
    g_object_unref(all);
    g_object_unref(filters);
    return dlg;
}

static const char *const ANY_EXTS[] = { "a52", "bin", "rom", "car", "zip", "u8", "a8", NULL };
static const char *const BIOS_EXTS[] = { "rom", "bin", "u8", "a8", "zip", NULL };
static const char *const ZIP_EXTS[] = { "zip", NULL };

/* Where to look for media: the folder the last file came from. */
static char *media_folder(A52Window *self)
{
    const char *last = a52session_get_str(self->session, "last_dir", "");
    if (last && *last && g_file_test(last, G_FILE_TEST_IS_DIR))
        return g_strdup(last);
    return g_strdup(a52session_media_path(self->session));
}

static void remember_folder(A52Window *self, const char *path)
{
    g_autofree char *dir = g_path_get_dirname(path);
    a52session_set_str(self->session, "last_dir", dir);
}

static void open_folder(A52Window *self, const char *path)
{
    g_autoptr(GFile) dir = g_file_new_for_path(path);
    GtkFileLauncher *l = gtk_file_launcher_new(dir);
    g_mkdir_with_parents(path, 0755);
    gtk_file_launcher_launch(l, GTK_WINDOW(self), NULL, NULL, NULL);
    g_object_unref(l);
}

/* ---- the keyboard ------------------------------------------------------------------ */

static void run_sysaction(A52Window *self, int sa);

/* The physical key as the session names it; GDK's keyval when the keycode
 * means nothing to it. */
static uint32_t keysym_of(guint keyval, guint keycode)
{
    uint32_t k = keycode >= 8 ? a52session_keysym_from_evdev(keycode - 8) : 0;
    if (k)
        return k;
    keyval = gdk_keyval_to_lower(keyval);
    return keyval < 0x100 || (keyval >= 0xff00 && keyval <= 0xffff) ? keyval : 0;
}

gboolean a52_window_key(A52Window *self, guint keyval, guint keycode,
                         GdkModifierType state, gboolean down)
{
    gboolean ctrl = (state & GDK_CONTROL_MASK) != 0;
    gboolean shift = (state & GDK_SHIFT_MASK) != 0;
    uint32_t keysym = keysym_of(keyval, keycode);

    /* the window's own keys */
    switch (keyval) {
    case GDK_KEY_F9:
        if (down)
            a52_controllers_toggle(GTK_WINDOW(self), self->session);
        return TRUE;
    case GDK_KEY_F11:
        if (down)
            gtk_widget_activate_action(GTK_WIDGET(self), "win.fullscreen", NULL);
        return TRUE;
    case GDK_KEY_F12:
        if (down)
            a52_debugger_toggle(GTK_WINDOW(self), self->session);
        return TRUE;
    case GDK_KEY_F5:
        if (shift) {
            if (down)
                gtk_widget_activate_action(GTK_WIDGET(self), "win.power-cycle", NULL);
            return TRUE;
        }
        break;
    case GDK_KEY_F1:
    case GDK_KEY_F6:
        if (down)
            gtk_widget_activate_action(GTK_WIDGET(self), "win.help", NULL);
        return TRUE;
    default:
        break;
    }
    if (!down) {
        /* always: a key let go while a shortcut was held must not stick */
        a52session_key(self->session, keysym, 0);
        return keysym != 0;
    }
    /* CTRL+SHIFT, ALT and SUPER are the app's and the desktop's, and CTRL+,
     * is Preferences */
    if ((state & (GDK_ALT_MASK | GDK_SUPER_MASK | GDK_META_MASK)) || (ctrl && shift &&
        keysym != A52_KEYSYM_LSHIFT && keysym != A52_KEYSYM_RSHIFT &&
        keysym != A52_KEYSYM_LCTRL && keysym != A52_KEYSYM_RCTRL) ||
        (ctrl && keyval == GDK_KEY_comma))
        return FALSE;
    if (!keysym)
        return FALSE;
    {
        int sa = a52session_key_sysaction(self->session, keysym);
        if (sa >= 0) {
            run_sysaction(self, sa);
            return TRUE;
        }
    }
    return a52session_key(self->session, keysym, 1) != 0;
}

/* An open dialog (Preferences, About, an alert) or menu has the keyboard:
 * Escape closes it and the arrows and Return move through it, instead of
 * going to the console. The key capture sits on the window, ahead of them. */
static gboolean ui_has_keyboard(A52Window *self)
{
    GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(self));
    if (adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(self)))
        return TRUE;
    return focus && gtk_widget_get_ancestor(focus, GTK_TYPE_POPOVER) != NULL;
}

static gboolean on_key_pressed(GtkEventControllerKey *ctrl, guint keyval, guint keycode,
                               GdkModifierType state, gpointer user_data)
{
    A52Window *self = user_data;
    (void)ctrl;
    /* with Preferences or a dialog open, F1 is Help (F6 is the Atari's HELP) */
    if (keyval == GDK_KEY_F1 && ui_has_keyboard(self) &&
        !(state & GDK_MODIFIER_MASK & ~GDK_LOCK_MASK)) {
        a52_help_for(GTK_WINDOW(self));
        return TRUE;
    }
    if (ui_has_keyboard(self))
        return FALSE;
    return a52_window_key(self, keyval, keycode, state, TRUE);
}

static void on_key_released(GtkEventControllerKey *ctrl, guint keyval, guint keycode,
                            GdkModifierType state, gpointer user_data)
{
    (void)ctrl;
    a52_window_key(user_data, keyval, keycode, state, FALSE);
}

/* Focus gone: nothing stays held. */
static void on_active_changed(GObject *win, GParamSpec *ps, gpointer user_data)
{
    A52Window *self = user_data;
    (void)ps;
    if (!gtk_window_is_active(GTK_WINDOW(win)) && !a52_controllers_visible())
        a52session_release_all(self->session);
}

/* ---- starting ------------------------------------------------------------------------ */

static void started(A52Window *self)
{
    self->menu_sig[0] = '\0';
    rebuild_menu(self, TRUE);
    update_status(self);
}

void a52_window_start(A52Window *self)
{
    a52session_start_opts opts;

    if (a52session_is_running(self->session))
        return;
    a52session_default_opts(self->session, &opts);
    if (a52session_start(self->session, &opts) != 0) {
        show_message(GTK_WIDGET(self), "Cannot Start the 5200",
                     a52session_last_error(self->session));
        update_status(self);
        return;
    }
    started(self);
}

/* ---- status, and news from the other threads ------------------------------------------- */

static void set_dot(GtkWidget *dot, gboolean on)
{
    gtk_widget_remove_css_class(dot, on ? "a52-dot-off" : "a52-dot-on");
    gtk_widget_add_css_class(dot, on ? "a52-dot-on" : "a52-dot-off");
}

static void update_status(A52Window *self)
{
    char text[512];
    if (!a52session_is_running(self->session)) {
        gtk_label_set_text(GTK_LABEL(self->status), "Off");
        set_dot(self->status_dot, FALSE);
        return;
    }
    a52session_status(self->session, text, sizeof text);
    gtk_label_set_text(GTK_LABEL(self->status), text);
    set_dot(self->status_dot, a52session_fujinet_link(self->session));
}

static gboolean status_tick(gpointer user_data)
{
    A52Window *self = user_data;
    update_status(self);
    rebuild_menu(self, FALSE);
    return G_SOURCE_CONTINUE;
}

static void on_notice_response(AdwAlertDialog *dlg, const char *response, gpointer user_data)
{
    A52Window *self = user_data;
    (void)dlg;
    self->notice_open = FALSE;
    if (!g_strcmp0(response, "debugger"))
        a52_debugger_show(GTK_WINDOW(self), self->session);
    else if (!g_strcmp0(response, "power"))
        gtk_widget_activate_action(GTK_WIDGET(self), "win.power-cycle", NULL);
}

static void show_notice(A52Window *self, const char *text)
{
    AdwDialog *dlg = adw_alert_dialog_new("The 5200 Stopped", text);
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dlg), "ok", "Leave It");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dlg), "debugger", "Open the Debugger");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dlg), "power", "Power Cycle");
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dlg), "power",
                                             ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dlg), "power");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dlg), "ok");
    g_signal_connect(dlg, "response", G_CALLBACK(on_notice_response), self);
    self->notice_open = TRUE;
    adw_dialog_present(dlg, GTK_WIDGET(self));
}

/* The gamepad thread cannot call into GTK; it posts system actions and this
 * timer takes them. It also says when gamepads come and go (a pad that drops
 * off Bluetooth mid-game should not be a mystery), when FujiNet restarted
 * itself, and when the 5200 crashed with no debugger open. */
static gboolean fast_tick(gpointer user_data)
{
    A52Window *self = user_data;
    char text[512];
    unsigned gen;
    int sa;

    while (a52session_sysaction_take(self->session, &sa))
        run_sysaction(self, sa);
    gen = a52session_gamepad_generation(self->session);
    if (gen != self->pad_generation) {
        self->pad_generation = gen;
        if (a52session_gamepad_last_event(self->session, text, sizeof text) > 0)
            a52_window_toast(self, text);
    }
    gen = a52session_fujinet_generation(self->session);
    if (gen != self->fujinet_generation) {
        self->fujinet_generation = gen;
        if (a52session_fujinet_last_event(self->session, text, sizeof text) > 0)
            a52_window_toast(self, text);
        update_status(self);
    }
    if (!self->notice_open && a52session_take_notice(self->session, text, sizeof text) > 0)
        show_notice(self, text);

    if (self->shot_path &&
        a52_display_frames(A52_DISPLAY(self->display)) >= self->shot_frames) {
        if (!a52_display_save_png(A52_DISPLAY(self->display), self->shot_path))
            g_printerr("A52_SCREENSHOT: could not write %s\n", self->shot_path);
        g_clear_pointer(&self->shot_path, g_free);
    }
    if (self->quit_frames && a52_display_frames(A52_DISPLAY(self->display)) >= self->quit_frames) {
        self->quit_frames = 0;
        gtk_window_close(GTK_WINDOW(self));
    }
    return G_SOURCE_CONTINUE;
}

/* ---- opening things ------------------------------------------------------------------ */

void a52_window_load_media(A52Window *self, const char *path)
{
    char msg[1024];
    int kind;

    if (!a52session_is_running(self->session)) {
        a52_window_toast(self, "The 5200 is off");
        return;
    }
    msg[0] = '\0';
    kind = a52session_open_or_import(self->session, path, msg, sizeof msg);
    if (kind < 0) {
        show_message(GTK_WIDGET(self), "Cannot Open It",
                     msg[0] ? msg : a52session_last_error(self->session));
        return;
    }
    remember_folder(self, path);
    if (kind == A52_MEDIA_BIOS)
        show_message(GTK_WIDGET(self), "BIOS Imported",
                     msg[0] ? msg : "Choose it in Preferences \xe2\x96\xb8 BIOS.");
    else
        a52_window_toast(self, msg[0] ? msg : base_name(path));
    rebuild_menu(self, FALSE);
}

static void on_open_chosen(GObject *src, GAsyncResult *res, gpointer user_data)
{
    A52Window *self = user_data;
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = file ? g_file_get_path(file) : NULL;
    if (path)
        a52_window_load_media(self, path);
}

static void action_open(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A52Window *self = user_data;
    g_autofree char *folder = media_folder(self);
    GtkFileDialog *dlg = file_dialog("Open", "5200 cartridges and BIOS images",
                                     ANY_EXTS, folder);
    (void)a; (void)p;
    gtk_file_dialog_open(dlg, GTK_WINDOW(self), NULL, on_open_chosen, self);
    g_object_unref(dlg);
}

static gboolean on_drop(GtkDropTarget *t, const GValue *value, double x, double y,
                        gpointer user_data)
{
    A52Window *self = user_data;
    g_autofree char *path = NULL;
    (void)t; (void)x; (void)y;
    if (!G_VALUE_HOLDS(value, G_TYPE_FILE))
        return FALSE;
    path = g_file_get_path(G_FILE(g_value_get_object(value)));
    if (!path)
        return FALSE;
    a52_window_load_media(self, path);
    return TRUE;
}

/* ---- BIOS import ------------------------------------------------------------------------ */

typedef struct {
    A52Window *self;
    void (*done)(gpointer);
    gpointer done_data;
    int slot;                   /* -1: everything recognised */
} BiosCtx;

static void bios_done(BiosCtx *c)
{
    rebuild_menu(c->self, TRUE);
    if (c->done)
        c->done(c->done_data);
    g_free(c);
}

/* One path: the session recognises files, zips and folders by size + CRC and
 * says what it took and what it ignored. */
static int bios_import_path(BiosCtx *c, const char *path, GString *log)
{
    char msg[1024];
    int n = a52session_rom_import(c->self->session, path, c->slot, msg, sizeof msg);
    if (log->len)
        g_string_append(log, "\n\n");
    g_string_append(log, msg);
    return n > 0 ? n : 0;
}

static void bios_report(BiosCtx *c, int imported, GString *log)
{
    show_message(GTK_WIDGET(c->self), imported ? "BIOS Imported" : "Nothing Imported",
                 log->len ? log->str : "Nothing was recognised.");
    if (imported)
        a52_window_toast(c->self, "Choose it in Preferences \xe2\x96\xb8 BIOS");
    g_string_free(log, TRUE);
    bios_done(c);
}

static void on_bios_files(GObject *src, GAsyncResult *res, gpointer user_data)
{
    BiosCtx *c = user_data;
    g_autoptr(GListModel) files =
        gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(src), res, NULL);
    GString *log = g_string_new(NULL);
    int imported = 0;

    if (!files) {
        g_string_free(log, TRUE);
        g_free(c);
        return;
    }
    for (guint i = 0; i < g_list_model_get_n_items(files); i++) {
        g_autoptr(GFile) f = g_list_model_get_item(files, i);
        g_autofree char *path = g_file_get_path(f);
        if (path)
            imported += bios_import_path(c, path, log);
    }
    bios_report(c, imported, log);
}

static void on_bios_zip(GObject *src, GAsyncResult *res, gpointer user_data)
{
    BiosCtx *c = user_data;
    g_autoptr(GFile) f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = f ? g_file_get_path(f) : NULL;
    GString *log;
    int n;

    if (!path) {
        g_free(c);
        return;
    }
    log = g_string_new(NULL);
    n = bios_import_path(c, path, log);
    bios_report(c, n, log);
}

static void on_bios_folder(GObject *src, GAsyncResult *res, gpointer user_data)
{
    BiosCtx *c = user_data;
    g_autoptr(GFile) f = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = f ? g_file_get_path(f) : NULL;
    GString *log;
    int n;

    if (!path) {
        g_free(c);
        return;
    }
    log = g_string_new(NULL);
    n = bios_import_path(c, path, log);
    bios_report(c, n, log);
}

/* kind 0: files (BIOS images or ZIPs, several at once), 1: one ZIP, 2: a
 * folder. slot >= 0 imports into that slot only (the ROMs list's Import).
 * done runs after the import, whatever came of it. */
void a52_window_import_bios(A52Window *self, int kind, int slot,
                            void (*done)(gpointer), gpointer done_data)
{
    BiosCtx *c = g_new0(BiosCtx, 1);
    g_autofree char *folder = media_folder(self);
    GtkFileDialog *dlg;

    c->self = self;
    c->slot = slot;
    c->done = done;
    c->done_data = done_data;
    if (kind == 2) {
        dlg = gtk_file_dialog_new();
        gtk_file_dialog_set_title(dlg, "Import BIOS from a Folder");
        gtk_file_dialog_select_folder(dlg, GTK_WINDOW(self), NULL, on_bios_folder, c);
    } else if (kind == 1) {
        dlg = file_dialog("Import BIOS from a ZIP", "ZIP files (MAME's a5200.zip)", ZIP_EXTS,
                          folder);
        gtk_file_dialog_open(dlg, GTK_WINDOW(self), NULL, on_bios_zip, c);
    } else {
        dlg = file_dialog(slot >= 0 ? "Import This BIOS" : "Import BIOS Files",
                          "BIOS images (*.rom, *.bin, *.u8, *.a8, *.zip)", BIOS_EXTS, folder);
        if (slot >= 0)
            gtk_file_dialog_open(dlg, GTK_WINDOW(self), NULL, on_bios_zip, c);
        else
            gtk_file_dialog_open_multiple(dlg, GTK_WINDOW(self), NULL, on_bios_files, c);
    }
    g_object_unref(dlg);
}

static void action_import_bios(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    (void)a;
    a52_window_import_bios(user_data, g_variant_get_int32(p), -1, NULL, NULL);
}

static void on_import_sd_chosen(GObject *src, GAsyncResult *res, gpointer user_data)
{
    A52Window *self = user_data;
    g_autoptr(GListModel) files =
        gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(src), res, NULL);
    int ok = 0, failed = 0;
    char t[200];
    if (!files)
        return;
    for (guint i = 0; i < g_list_model_get_n_items(files); i++) {
        g_autoptr(GFile) f = g_list_model_get_item(files, i);
        g_autofree char *path = g_file_get_path(f);
        char dest[1024];
        if (path && a52session_import_to_sd(self->session, path, dest, sizeof dest) == 0)
            ok++;
        else
            failed++;
    }
    if (failed)
        g_snprintf(t, sizeof t, "%d copied to FujiNet's SD card, %d could not be", ok, failed);
    else
        g_snprintf(t, sizeof t, "%d copied to FujiNet's SD card: pick it from CONFIG", ok);
    a52_window_toast(self, t);
}

static void action_import_sd(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A52Window *self = user_data;
    g_autofree char *folder = media_folder(self);
    GtkFileDialog *dlg = file_dialog("Import to FujiNet's SD Card",
                                     "Games and disk images", NULL, folder);
    (void)a; (void)p;
    gtk_file_dialog_open_multiple(dlg, GTK_WINDOW(self), NULL, on_import_sd_chosen, self);
    g_object_unref(dlg);
}

static void action_sd_folder(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A52Window *self = user_data;
    (void)a; (void)p;
    open_folder(self, a52session_sd_path(self->session));
}

static void action_media_folder(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A52Window *self = user_data;
    (void)a; (void)p;
    open_folder(self, a52session_media_path(self->session));
}

/* ---- the machine and the console keys ----------------------------------------------------------- */

static void run_sysaction(A52Window *self, int sa)
{
    switch (sa) {
    case A52_SYSACT_PAUSE:
        a52_debugger_show(GTK_WINDOW(self), self->session);
        break;
    case A52_SYSACT_POWER:
        gtk_widget_activate_action(GTK_WIDGET(self), "win.power-cycle", NULL);
        break;
    case A52_SYSACT_WARP:
        gtk_widget_activate_action(GTK_WIDGET(self), "win.warp", NULL);
        break;
    default:
        break;
    }
}

static void action_power_cycle(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A52Window *self = user_data;
    (void)a; (void)p;
    if (!a52session_is_running(self->session)) {
        a52_window_start(self);
        return;
    }
    if (a52session_power_cycle(self->session) != 0)
        show_message(GTK_WIDGET(self), "Cannot Power Cycle", a52session_last_error(self->session));
    update_status(self);
}

static void action_reset(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    (void)a; (void)p;
    a52session_reset(A52_WINDOW(user_data)->session);
}

static void action_reboot_config(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A52Window *self = user_data;
    (void)a; (void)p;
    if (a52session_reboot_to_config(self->session) == 0)
        a52_window_toast(self, "Back to FujiNet's CONFIG");
    rebuild_menu(self, FALSE);
}

static void action_warp(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A52Window *self = user_data;
    gboolean on = !g_variant_get_boolean(g_action_get_state(G_ACTION(a)));
    (void)p;
    g_simple_action_set_state(a, g_variant_new_boolean(on));
    a52session_set_warp(self->session, on);
    update_status(self);
}

static void action_prefs(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A52Window *self = user_data;
    (void)a; (void)p;
    a52_prefs_show(self, self->session);
}

static void action_quit(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    (void)a; (void)p;
    gtk_window_close(GTK_WINDOW(user_data));
}

/* ---- the view ---------------------------------------------------------------------------------- */

static void action_controllers(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A52Window *self = user_data;
    (void)a; (void)p;
    a52_controllers_toggle(GTK_WINDOW(self), self->session);
}

static void action_debugger(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A52Window *self = user_data;
    (void)a; (void)p;
    a52_debugger_toggle(GTK_WINDOW(self), self->session);
}

static void action_fullscreen(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A52Window *self = user_data;
    (void)a; (void)p;
    self->fullscreen = !self->fullscreen;
    if (self->fullscreen)
        gtk_window_fullscreen(GTK_WINDOW(self));
    else
        gtk_window_unfullscreen(GTK_WINDOW(self));
}

static void set_pref_choice(a52session *s, const char *key, int c)
{
    int i = a52session_pref_find(key);
    if (i >= 0)
        a52session_pref_set_choice(s, i, c);
}

static void action_aspect(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A52Window *self = user_data;
    gboolean tv = !g_variant_get_boolean(g_action_get_state(G_ACTION(a)));
    (void)p;
    set_pref_choice(self->session, "aspect", tv ? 0 : 1);
    a52_window_apply_view(self);
}

static void action_smooth(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A52Window *self = user_data;
    gboolean sm = !g_variant_get_boolean(g_action_get_state(G_ACTION(a)));
    (void)p;
    a52session_set_int(self->session, "smooth", sm);
    a52_window_apply_view(self);
}

static void set_toggle(A52Window *self, const char *name, gboolean on)
{
    GAction *a = g_action_map_lookup_action(G_ACTION_MAP(self), name);
    if (a)
        g_simple_action_set_state(G_SIMPLE_ACTION(a), g_variant_new_boolean(on));
}

static void set_enabled(A52Window *self, const char *name, gboolean on)
{
    GAction *a = g_action_map_lookup_action(G_ACTION_MAP(self), name);
    if (a)
        g_simple_action_set_enabled(G_SIMPLE_ACTION(a), on);
}

void a52_window_set_stick_port(A52Window *self, int port)
{
    a52_display_set_stick_port(A52_DISPLAY(self->display), port);
}

void a52_window_apply_view(A52Window *self)
{
    int i = a52session_pref_find("aspect");
    gboolean tv = i < 0 || a52session_pref_get_choice(self->session, i) == 0;
    gboolean smooth = a52session_get_int(self->session, "smooth", 0) != 0;
    a52_display_set_tv_aspect(A52_DISPLAY(self->display), tv);
    a52_display_set_smooth(A52_DISPLAY(self->display), smooth);
    set_toggle(self, "tv-aspect", tv);
    set_toggle(self, "smooth", smooth);
}

void a52_window_settings_changed(A52Window *self)
{
    a52_window_apply_view(self);
    a52_controllers_refresh();
    rebuild_menu(self, TRUE);
    update_status(self);
}

/* ---- FujiNet ------------------------------------------------------------------------------------- */

static void action_fujinet_config(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A52Window *self = user_data;
    (void)a; (void)p;
    if (a52session_fujinet_external(self->session)) {
        a52_window_toast(self, "FujiNet is external: use its own web UI");
        return;
    }
    if (!a52session_fujinet_running(self->session)) {
        a52_window_toast(self, "FujiNet is off (Preferences \xe2\x96\xb8 FujiNet)");
        return;
    }
    a52_fujiconfig_show(GTK_WINDOW(self), self->session);
}

static void action_fujinet_log(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A52Window *self = user_data;
    (void)a; (void)p;
    if (a52session_fujinet_external(self->session)) {
        a52_window_toast(self, "FujiNet is external: see its own console");
        return;
    }
    a52_fujilog_show(GTK_WINDOW(self), self->session);
}

static void action_fujinet_restart(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A52Window *self = user_data;
    (void)a; (void)p;
    if (a52session_fujinet_external(self->session)) {
        a52_window_toast(self, "FujiNet is external: restart it where it runs");
        return;
    }
    if (a52session_fujinet_restart(self->session) != 0)
        show_message(GTK_WIDGET(self), "Cannot Restart FujiNet",
                     a52session_last_error(self->session));
    else
        a52_window_toast(self, "FujiNet restarted");
}

static void action_about(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A52Window *self = user_data;
    AdwDialog *about;
    (void)a; (void)p;
    about = adw_about_dialog_new();
    adw_about_dialog_set_application_name(ADW_ABOUT_DIALOG(about), "FujiNet Go Atari 5200");
    adw_about_dialog_set_application_icon(ADW_ABOUT_DIALOG(about), a52_icon_name());
    adw_about_dialog_set_version(ADW_ABOUT_DIALOG(about), A52_VERSION_STRING);
    adw_about_dialog_set_developer_name(ADW_ABOUT_DIALOG(about), "Thomas Cherryhomes");
    adw_about_dialog_set_website(ADW_ABOUT_DIALOG(about), "https://fujinet.online/");
    adw_about_dialog_set_issue_url(ADW_ABOUT_DIALOG(about),
        "https://github.com/FujiNetWIFI/fujinet-go-atari5200-desktop/issues");
    adw_about_dialog_set_license_type(ADW_ABOUT_DIALOG(about), GTK_LICENSE_GPL_3_0);
    adw_about_dialog_set_comments(ADW_ABOUT_DIALOG(about),
        "The Atari 5200 SuperSystem with the FujiNet cartridge in its slot. The "
        "emulator is atari800 (GPL-2.0-or-later, the Atari800 Development Team) "
        "in 5200 mode; the cartridge is the FujiNet firmware's own (GPL-3.0-or-later), "
        "talking BoIP FujiBus to FujiNet. The Altirra 5200 OS (Avery Lee) is "
        "built in; Atari's own BIOS can be imported.");
    adw_dialog_present(about, GTK_WIDGET(self));
}

/* The app's short list of keys (F6); Help proper is app.help. */
static void action_keys(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A52Window *self = user_data;
    (void)a; (void)p;
    show_message(GTK_WIDGET(self), "FujiNet Go Atari 5200",
        "Power Cycle (Shift+F5) is the console's power switch: it has no reset line, "
        "and a power cycle brings FujiNet's CONFIG back.\n\n"
        "Open a cartridge image (.a52 .bin .rom .car, or a ZIP) from the menu or by "
        "dropping it on the window; a BIOS file, ZIP or folder is imported instead.\n\n"
        "Controller 1: arrow keys move the analog stick, Space is fire, the digit row "
        "is the keypad, with START, PAUSE and RESET on the function keys (see the "
        "Controllers window, F9, to see and change them). Dragging with the mouse in "
        "the picture moves the stick of the controller chosen there.\n\n"
        "F9 Controllers, F11 Fullscreen, F12 Debugger, Ctrl+Shift+O Open, "
        "Ctrl+Shift+W Warp, Ctrl+Shift+Q Quit.");
}

const char *a52_window_help_context(A52Window *self)
{
    static char ctx[64];
    AdwDialog *d = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(self));

    if (d && ADW_IS_PREFERENCES_DIALOG(d)) {
        const char *page = adw_preferences_dialog_get_visible_page_name(ADW_PREFERENCES_DIALOG(d));
        if (page) {
            g_snprintf(ctx, sizeof ctx, "prefs/%s", page);
            return ctx;
        }
    }
    return "main";
}

/* app.help: every window's Ctrl+Shift+H, for what it shows. Not a cast of
 * the active window: it can be the debugger, the controllers or the log. */
static void action_help(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    (void)a; (void)p;
    a52_help_for(gtk_application_get_active_window(GTK_APPLICATION(user_data)));
}

static const GActionEntry app_actions[] = {
    { "help", action_help, NULL, NULL, NULL, { 0 } },
};

static const GActionEntry win_actions[] = {
    { "open", action_open, NULL, NULL, NULL, { 0 } },
    { "power-cycle", action_power_cycle, NULL, NULL, NULL, { 0 } },
    { "reset", action_reset, NULL, NULL, NULL, { 0 } },
    { "reboot-config", action_reboot_config, NULL, NULL, NULL, { 0 } },
    { "help", action_keys, NULL, NULL, NULL, { 0 } },
    { "warp", action_warp, NULL, "false", NULL, { 0 } },
    { "import-bios", action_import_bios, "i", NULL, NULL, { 0 } },
    { "import-sd", action_import_sd, NULL, NULL, NULL, { 0 } },
    { "sd-folder", action_sd_folder, NULL, NULL, NULL, { 0 } },
    { "media-folder", action_media_folder, NULL, NULL, NULL, { 0 } },
    { "controllers", action_controllers, NULL, NULL, NULL, { 0 } },
    { "debugger", action_debugger, NULL, NULL, NULL, { 0 } },
    { "tv-aspect", action_aspect, NULL, "true", NULL, { 0 } },
    { "smooth", action_smooth, NULL, "false", NULL, { 0 } },
    { "fullscreen", action_fullscreen, NULL, NULL, NULL, { 0 } },
    { "fujinet-config", action_fujinet_config, NULL, NULL, NULL, { 0 } },
    { "fujinet-log", action_fujinet_log, NULL, NULL, NULL, { 0 } },
    { "fujinet-restart", action_fujinet_restart, NULL, NULL, NULL, { 0 } },
    { "prefs", action_prefs, NULL, NULL, NULL, { 0 } },
    { "about", action_about, NULL, NULL, NULL, { 0 } },
    { "quit", action_quit, NULL, NULL, NULL, { 0 } },
};

/* ---- the menu ------------------------------------------------------------------------------------- */

static GMenu *build_menu(A52Window *self)
{
    gboolean fuji = a52session_fujinet_running(self->session);
    GMenu *menu = g_menu_new();
    GMenu *machine = g_menu_new(), *sd = g_menu_new(), *bios = g_menu_new();
    GMenu *view = g_menu_new(), *fujim = g_menu_new(), *app = g_menu_new();
    char info[256], label[300];

    g_menu_append(machine, "_Open\xe2\x80\xa6", "win.open");
    if (a52session_cart_info(self->session, info, sizeof info) > 0) {
        g_autofree char *e = menu_escape(info);
        g_snprintf(label, sizeof label, "In the slot: %s", e);
        g_menu_append(machine, label, "win.cart-noop");
    }
    g_menu_append(machine, "_Power Cycle (Shift+F5)", "win.power-cycle");
    g_menu_append(machine, "Reboot to _CONFIG", "win.reboot-config");
    g_menu_append(machine, "_RESET Key (F5)", "win.reset");
    g_menu_append(machine, "_Warp Speed", "win.warp");
    g_menu_append_section(menu, "Console", G_MENU_MODEL(machine));

    g_menu_append(bios, "From _Files\xe2\x80\xa6", "win.import-bios(0)");
    g_menu_append(bios, "From a _ZIP\xe2\x80\xa6", "win.import-bios(1)");
    g_menu_append(bios, "From a Fol_der\xe2\x80\xa6", "win.import-bios(2)");
    g_menu_append_submenu(sd, "Import _BIOS", G_MENU_MODEL(bios));
    if (fuji) {
        g_menu_append(sd, "_Import to SD Card\xe2\x80\xa6", "win.import-sd");
        g_menu_append(sd, "Open _SD Folder", "win.sd-folder");
    }
    g_menu_append(sd, "Open _Media Folder", "win.media-folder");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(sd));

    g_menu_append(view, "C_ontrollers (F9)", "win.controllers");
    g_menu_append(view, "De_bugger (F12)", "win.debugger");
    g_menu_append(view, "_TV Aspect", "win.tv-aspect");
    g_menu_append(view, "S_mooth Scaling", "win.smooth");
    g_menu_append(view, "_Fullscreen (F11)", "win.fullscreen");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(view));

    g_menu_append(fujim, "FujiNet _Web UI", "win.fujinet-config");
    g_menu_append(fujim, "Console _Log", "win.fujinet-log");
    g_menu_append(fujim, "R_estart FujiNet", "win.fujinet-restart");
    g_menu_append_section(menu, "FujiNet", G_MENU_MODEL(fujim));

    g_menu_append(app, "_Preferences", "win.prefs");
    g_menu_append(app, "_Help", "app.help");
    g_menu_append(app, "_Keys (F6)", "win.help");
    g_menu_append(app, "_About FujiNet Go Atari 5200", "win.about");
    g_menu_append(app, "_Quit", "win.quit");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(app));

    g_object_unref(machine);
    g_object_unref(sd);
    g_object_unref(bios);
    g_object_unref(view);
    g_object_unref(fujim);
    g_object_unref(app);
    return menu;
}

/* What the media menus show, to rebuild only on a change: GtkPopoverMenu
 * names its submenu pages by label, and changing the model it shows in
 * place re-adds pages whose names are still taken. */
static void rebuild_menu(A52Window *self, gboolean force)
{
    GString *sig = g_string_new(NULL);
    gboolean fuji = a52session_fujinet_running(self->session);
    char info[256];

    a52session_cart_info(self->session, info, sizeof info);
    g_string_append_printf(sig, "%d|%s", fuji, info);

    set_enabled(self, "fujinet-config", fuji || a52session_fujinet_external(self->session));
    set_enabled(self, "fujinet-restart", a52session_get_int(self->session, "enable_fujinet", 1));

    if (force || strcmp(sig->str, self->menu_sig) != 0) {
        GMenu *menu = build_menu(self);
        g_strlcpy(self->menu_sig, sig->str, sizeof self->menu_sig);
        gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(self->menu_button), G_MENU_MODEL(menu));
        g_object_unref(menu);
    }
    g_string_free(sig, TRUE);
}

/* ---- construction ---------------------------------------------------------------------------------- */

static void a52_window_dispose(GObject *object)
{
    A52Window *self = A52_WINDOW(object);
    if (self->status_id) {
        g_source_remove(self->status_id);
        self->status_id = 0;
    }
    if (self->fast_id) {
        g_source_remove(self->fast_id);
        self->fast_id = 0;
    }
    g_clear_pointer(&self->shot_path, g_free);
    G_OBJECT_CLASS(a52_window_parent_class)->dispose(object);
}

static void a52_window_class_init(A52WindowClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = a52_window_dispose;
}

static void a52_window_init(A52Window *self)
{
    (void)self;
}

static GtkWidget *dot(const char *tip)
{
    GtkWidget *d = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(d, "a52-dot");
    gtk_widget_add_css_class(d, "a52-dot-off");
    gtk_widget_set_valign(d, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(d, tip);
    return d;
}

static gboolean env_on(const char *name)
{
    const char *v = g_getenv(name);
    return v && *v && *v != '0';
}

static void noop(GSimpleAction *a, GVariant *p, gpointer u)
{
    (void)a; (void)p; (void)u;
}

GtkWidget *a52_window_new(AdwApplication *app, a52session *session)
{
    A52Window *self = g_object_new(A52_TYPE_WINDOW, "application", app, NULL);
    GtkWidget *header, *menu_button, *toolbar, *status_box;
    GtkEventController *keys;

    self->session = session;

    gtk_window_set_title(GTK_WINDOW(self), "FujiNet Go Atari 5200");
    gtk_window_set_icon_name(GTK_WINDOW(self), a52_icon_name());
    /* 336 x 240 at three times, a TV's shape: 864 x 720 */
    gtk_window_set_default_size(GTK_WINDOW(self), 864, 720 + 46);

    g_action_map_add_action_entries(G_ACTION_MAP(self), win_actions, G_N_ELEMENTS(win_actions),
                                    self);
    if (!g_action_map_lookup_action(G_ACTION_MAP(app), "help"))
        g_action_map_add_action_entries(G_ACTION_MAP(app), app_actions,
                                        G_N_ELEMENTS(app_actions), app);
    {
        /* information rows in the menu: shown, never active */
        GSimpleAction *info = g_simple_action_new("cart-noop", NULL);
        g_simple_action_set_enabled(info, FALSE);
        g_signal_connect(info, "activate", G_CALLBACK(noop), NULL);
        g_action_map_add_action(G_ACTION_MAP(self), G_ACTION(info));
        g_object_unref(info);
    }
    {
        /* CTRL+SHIFT: CTRL alone is the console's */
        static const char *const prefs[] = { "<Control>comma", NULL };
        static const char *const quit[] = { "<Control><Shift>q", NULL };
        static const char *const open[] = { "<Control><Shift>o", NULL };
        static const char *const copy[] = { "<Control><Shift>c", NULL };
        static const char *const paste[] = { "<Control><Shift>v", NULL };
        static const char *const warp[] = { "<Control><Shift>w", NULL };
        static const char *const help[] = { "<Control><Shift>h", NULL };
        GtkApplication *a = GTK_APPLICATION(app);
        gtk_application_set_accels_for_action(a, "app.help", help);
        gtk_application_set_accels_for_action(a, "win.prefs", prefs);
        gtk_application_set_accels_for_action(a, "win.quit", quit);
        gtk_application_set_accels_for_action(a, "win.open", open);
        gtk_application_set_accels_for_action(a, "win.copy-screen", copy);
        gtk_application_set_accels_for_action(a, "win.paste", paste);
        gtk_application_set_accels_for_action(a, "win.warp", warp);
    }

    header = adw_header_bar_new();
    menu_button = gtk_menu_button_new();
    self->menu_button = menu_button;
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_button), "open-menu-symbolic");
    gtk_menu_button_set_primary(GTK_MENU_BUTTON(menu_button), TRUE);
    gtk_widget_set_tooltip_text(menu_button, "Main Menu");
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), menu_button);

    status_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    self->status_dot = dot("FujiNet link");
    self->status = gtk_label_new("Starting\xe2\x80\xa6");
    gtk_widget_add_css_class(self->status, "dim-label");
    gtk_label_set_ellipsize(GTK_LABEL(self->status), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(status_box), self->status_dot);
    gtk_box_append(GTK_BOX(status_box), self->status);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), status_box);

    self->display = a52_display_new(session);
    a52_window_apply_view(self);
    {
        GtkDropTarget *drop = gtk_drop_target_new(G_TYPE_FILE, GDK_ACTION_COPY);
        g_signal_connect(drop, "drop", G_CALLBACK(on_drop), self);
        gtk_widget_add_controller(self->display, GTK_EVENT_CONTROLLER(drop));
    }

    self->toast_overlay = adw_toast_overlay_new();
    adw_toast_overlay_set_child(ADW_TOAST_OVERLAY(self->toast_overlay), self->display);
    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), self->toast_overlay);
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(self), toolbar);

    /* capture on the WINDOW so typing works whatever has focus */
    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), self);
    g_signal_connect(keys, "key-released", G_CALLBACK(on_key_released), self);
    gtk_widget_add_controller(GTK_WIDGET(self), keys);
    g_signal_connect(self, "notify::is-active", G_CALLBACK(on_active_changed), self);

    /* the current pads and FujiNet state are the baseline: no toasts for them */
    self->pad_generation = a52session_gamepad_generation(session);
    self->fujinet_generation = a52session_fujinet_generation(session);
    rebuild_menu(self, TRUE);
    self->status_id = g_timeout_add_seconds(1, status_tick, self);
    self->fast_id = g_timeout_add(100, fast_tick, self);
    update_status(self);

    /* A52_OPEN_CONTROLLERS / _DEBUGGER / _SETTINGS open those windows at
     * launch, the family's way in when the app misbehaves before the menu
     * is reachable. A52_SCREENSHOT=file.png saves the picture after
     * A52_SCREENSHOT_FRAMES frames (300); A52_AUTOQUIT_FRAMES quits after
     * that many. */
    if (env_on("A52_OPEN_CONTROLLERS"))
        a52_controllers_toggle(GTK_WINDOW(self), session);
    if (env_on("A52_OPEN_DEBUGGER"))
        a52_debugger_show(GTK_WINDOW(self), session);
    if (env_on("A52_OPEN_SETTINGS"))
        a52_prefs_show(self, session);
    if (g_getenv("A52_SCREENSHOT") && *g_getenv("A52_SCREENSHOT")) {
        const char *n = g_getenv("A52_SCREENSHOT_FRAMES");
        self->shot_path = g_strdup(g_getenv("A52_SCREENSHOT"));
        self->shot_frames = n ? g_ascii_strtoull(n, NULL, 10) : 300;
    }
    if (g_getenv("A52_AUTOQUIT_FRAMES"))
        self->quit_frames = g_ascii_strtoull(g_getenv("A52_AUTOQUIT_FRAMES"), NULL, 10);
    return GTK_WIDGET(self);
}
