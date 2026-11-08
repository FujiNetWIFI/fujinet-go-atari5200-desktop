/*
 * The Preferences dialog: an AdwPreferencesDialog built from the session's
 * preference table (a52session_pref), so the four frontends show the same
 * pages, groups and rows. No .ui file.
 *
 * Rows that apply LIVE (the picture, the sound, the controllers) take effect
 * at once. The console -- TV system, BIOS -- is the machine you would switch
 * off to change: those rows wait, and closing the dialog applies them
 * together (one power cycle), as do the host options (FujiNet, sound,
 * gamepads: a session restart). The BIOS choice follows what has been
 * imported.
 *
 * The BIOS page also lists every BIOS slot, imported or not, each with its
 * own Import and Remove, and Import BIOS from Files / a ZIP / a Folder.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "prefs.h"
#include "help/help_window.h"

#include "controllers/controllers_window.h"

#include <string.h>

#define MAX_ROWS 128
#define MAX_ROM_ROWS 64

typedef struct {
    A52Window *window;
    a52session *session;
    AdwDialog *dialog;
    GtkWidget *rows[MAX_ROWS];          /* by pref index */
    char *labels[MAX_ROWS];             /* a choice row's labels, as shown */
    GtkWidget *rom_rows[MAX_ROM_ROWS];
    GtkWidget *rom_remove[MAX_ROM_ROWS];
    gboolean syncing;
    int pending;                        /* the heaviest change not applied yet */
} PrefsState;

static void refresh_rows(PrefsState *st);

static const char *page_icon(const char *page)
{
    if (!strcmp(page, "BIOS"))
        return "media-flash-symbolic";
    if (!strcmp(page, "Video"))
        return "video-display-symbolic";
    if (!strcmp(page, "Audio"))
        return "audio-speakers-symbolic";
    if (!strcmp(page, "Input"))
        return "input-gaming-symbolic";
    if (!strcmp(page, "FujiNet"))
        return "network-wireless-symbolic";
    return "applications-system-symbolic";
}

/* A change to pref i: LIVE ones apply now unless something heavier waits
 * (applying then would power-cycle mid-edit). */
static void changed(PrefsState *st, int i)
{
    const a52_pref *p = a52session_pref(i);
    if (p->applies > st->pending)
        st->pending = p->applies;
    if (st->pending == A52_APPLY_LIVE)
        a52session_apply_settings(st->session);
    if (!strcmp(p->key, "aspect") || !strcmp(p->key, "smooth"))
        a52_window_apply_view(st->window);
    if (!strcmp(p->key, "analog_stick") || !strcmp(p->key, "autofire"))
        a52_controllers_refresh();
    refresh_rows(st);
}

/* ---- rows -------------------------------------------------------------------- */

static void on_switch(GObject *row, GParamSpec *ps, gpointer ud)
{
    PrefsState *st = ud;
    int i = GPOINTER_TO_INT(g_object_get_data(row, "a52-pref"));
    (void)ps;
    if (st->syncing)
        return;
    a52session_set_int(st->session, a52session_pref(i)->key,
                        adw_switch_row_get_active(ADW_SWITCH_ROW(row)));
    changed(st, i);
}

static void on_spin(GObject *row, GParamSpec *ps, gpointer ud)
{
    PrefsState *st = ud;
    int i = GPOINTER_TO_INT(g_object_get_data(row, "a52-pref"));
    (void)ps;
    if (st->syncing)
        return;
    a52session_set_int(st->session, a52session_pref(i)->key,
                        (int)adw_spin_row_get_value(ADW_SPIN_ROW(row)));
    changed(st, i);
}

static void on_combo(GObject *row, GParamSpec *ps, gpointer ud)
{
    PrefsState *st = ud;
    int i = GPOINTER_TO_INT(g_object_get_data(row, "a52-pref"));
    guint c = adw_combo_row_get_selected(ADW_COMBO_ROW(row));
    (void)ps;
    if (st->syncing || c == GTK_INVALID_LIST_POSITION)
        return;
    if (!a52session_pref_choice_enabled(st->session, i, (int)c)) {
        /* not imported, or not on this machine: put it back */
        a52_window_toast(st->window, "Not available \xe2\x80\x94 import that BIOS first");
        refresh_rows(st);
        return;
    }
    a52session_pref_set_choice(st->session, i, (int)c);
    changed(st, i);
}

/* A text row: valid text is stored and marks the change; anything else is
 * flagged and not stored, so the last good text stays in force. */
static void on_text(GtkEditable *entry, gpointer ud)
{
    PrefsState *st = ud;
    int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(entry), "a52-pref"));
    const char *text = gtk_editable_get_text(entry);
    if (st->syncing)
        return;
    if (a52session_pref_set_text(st->session, i, text, NULL, 0) != 0) {
        gtk_widget_add_css_class(GTK_WIDGET(entry), "error");
        return;
    }
    gtk_widget_remove_css_class(GTK_WIDGET(entry), "error");
    changed(st, i);
}

typedef struct {
    PrefsState *st;
    int i;
} PathCtx;

static void on_path_chosen(GObject *src, GAsyncResult *res, gpointer ud)
{
    PathCtx *c = ud;
    const a52_pref *p = a52session_pref(c->i);
    g_autoptr(GFile) f = p->type == A52_PREF_DIR
        ? gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, NULL)
        : gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = f ? g_file_get_path(f) : NULL;
    if (path) {
        a52session_set_str(c->st->session, p->key, path);
        changed(c->st, c->i);
    }
    g_free(c);
}

static void on_path_choose(GtkButton *b, gpointer ud)
{
    PrefsState *st = ud;
    int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "a52-pref"));
    const a52_pref *p = a52session_pref(i);
    GtkFileDialog *dlg = gtk_file_dialog_new();
    PathCtx *c = g_new0(PathCtx, 1);
    const char *cur = a52session_get_str(st->session, p->key, "");
    c->st = st;
    c->i = i;
    gtk_file_dialog_set_title(dlg, p->label);
    if (cur && *cur) {
        g_autoptr(GFile) f = g_file_new_for_path(cur);
        if (p->type == A52_PREF_DIR)
            gtk_file_dialog_set_initial_folder(dlg, f);
        else
            gtk_file_dialog_set_initial_file(dlg, f);
    }
    if (p->type == A52_PREF_DIR)
        gtk_file_dialog_select_folder(dlg, GTK_WINDOW(st->window), NULL, on_path_chosen, c);
    else
        gtk_file_dialog_open(dlg, GTK_WINDOW(st->window), NULL, on_path_chosen, c);
    g_object_unref(dlg);
}

static void on_path_clear(GtkButton *b, gpointer ud)
{
    PrefsState *st = ud;
    int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "a52-pref"));
    a52session_set_str(st->session, a52session_pref(i)->key, "");
    changed(st, i);
}

static GtkWidget *icon_button(const char *icon, const char *tip, int i, GCallback cb,
                              PrefsState *st)
{
    GtkWidget *b = gtk_button_new_from_icon_name(icon);
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(b, "flat");
    gtk_widget_set_tooltip_text(b, tip);
    g_object_set_data(G_OBJECT(b), "a52-pref", GINT_TO_POINTER(i));
    g_signal_connect(b, "clicked", cb, st);
    return b;
}

static GtkWidget *make_row(PrefsState *st, int i)
{
    const a52_pref *p = a52session_pref(i);
    GtkWidget *row;

    switch (p->type) {
    case A52_PREF_BOOL:
        row = adw_switch_row_new();
        g_signal_connect(row, "notify::active", G_CALLBACK(on_switch), st);
        break;
    case A52_PREF_INT:
        row = adw_spin_row_new_with_range(p->min, p->max, p->step > 0 ? p->step : 1);
        g_signal_connect(row, "notify::value", G_CALLBACK(on_spin), st);
        break;
    case A52_PREF_CHOICE:
        row = adw_combo_row_new();
        g_signal_connect(row, "notify::selected", G_CALLBACK(on_combo), st);
        break;
    case A52_PREF_TEXT: {
        const char *hold = a52session_pref_placeholder(i);
        g_autofree char *title = g_strdup_printf(hold ? "%s, e.g. %s" : "%s", p->label, hold);
        row = adw_entry_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
        gtk_widget_set_tooltip_text(row, p->help);
        g_signal_connect(row, "changed", G_CALLBACK(on_text), st);
        g_object_set_data(G_OBJECT(row), "a52-pref", GINT_TO_POINTER(i));
        return row;
    }
    default:   /* a file or a folder */
        row = adw_action_row_new();
        adw_action_row_add_suffix(ADW_ACTION_ROW(row),
                                  icon_button("edit-clear-symbolic", "Clear", i,
                                              G_CALLBACK(on_path_clear), st));
        adw_action_row_add_suffix(ADW_ACTION_ROW(row),
                                  icon_button(p->type == A52_PREF_DIR ? "folder-open-symbolic"
                                                                       : "document-open-symbolic",
                                              "Choose\xe2\x80\xa6", i,
                                              G_CALLBACK(on_path_choose), st));
        break;
    }
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), p->label);
    if (p->help && p->type != A52_PREF_FILE && p->type != A52_PREF_DIR)
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), p->help);
    g_object_set_data(G_OBJECT(row), "a52-pref", GINT_TO_POINTER(i));
    return row;
}

/* A choice row's labels joined, to see when they change. */
static char *choice_labels(PrefsState *st, int i)
{
    GString *g = g_string_new(NULL);
    const char *l;
    for (int c = 0; (l = a52session_pref_choice(st->session, i, c)) != NULL; c++) {
        g_string_append(g, l);
        if (!a52session_pref_choice_enabled(st->session, i, c))
            g_string_append(g, " (not available)");
        g_string_append_c(g, '\n');
    }
    return g_string_free(g, FALSE);
}

/* Every row from the store: values, labels that depend on others, and
 * which rows apply to this machine. */
static void refresh_rows(PrefsState *st)
{
    st->syncing = TRUE;
    for (int i = 0; i < a52session_pref_count() && i < MAX_ROWS; i++) {
        const a52_pref *p = a52session_pref(i);
        GtkWidget *row = st->rows[i];
        if (!row)
            continue;
        gtk_widget_set_sensitive(row, a52session_pref_enabled(st->session, i));
        switch (p->type) {
        case A52_PREF_BOOL:
            adw_switch_row_set_active(ADW_SWITCH_ROW(row),
                                      a52session_get_int(st->session, p->key, p->def) != 0);
            break;
        case A52_PREF_INT:
            adw_spin_row_set_value(ADW_SPIN_ROW(row),
                                   a52session_get_int(st->session, p->key, p->def));
            break;
        case A52_PREF_CHOICE: {
            char *labels = choice_labels(st, i);
            int sel = a52session_pref_get_choice(st->session, i);
            if (g_strcmp0(labels, st->labels[i]) != 0) {
                char **parts = g_strsplit(labels, "\n", -1);
                guint n = g_strv_length(parts);
                if (n && !*parts[n - 1]) {
                    g_free(parts[n - 1]);
                    parts[n - 1] = NULL;
                }
                GtkStringList *model = gtk_string_list_new((const char *const *)parts);
                adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(model));
                g_object_unref(model);
                g_strfreev(parts);
                g_free(st->labels[i]);
                st->labels[i] = labels;
            } else {
                g_free(labels);
            }
            adw_combo_row_set_selected(ADW_COMBO_ROW(row), sel >= 0 ? (guint)sel : 0);
            break;
        }
        case A52_PREF_TEXT:
            /* the text is the user's own: filled in once, never rewritten mid-edit */
            if (!g_object_get_data(G_OBJECT(row), "a52-text-set")) {
                gtk_editable_set_text(GTK_EDITABLE(row), a52session_pref_get_text(st->session, i));
                g_object_set_data(G_OBJECT(row), "a52-text-set", GINT_TO_POINTER(1));
            }
            break;
        default: {
            const char *v = a52session_get_str(st->session, p->key, "");
            g_autofree char *base = v && *v ? g_path_get_basename(v) : NULL;
            adw_action_row_set_subtitle(ADW_ACTION_ROW(row),
                                        base ? base : (p->help ? p->help : "None"));
            gtk_widget_set_tooltip_text(row, v && *v ? v : NULL);
            break;
        }
        }
    }
    for (int r = 0; r < a52session_rom_slot_count() && r < MAX_ROM_ROWS; r++)
        if (st->rom_rows[r]) {
            gboolean have = a52session_rom_available(st->session, r) != 0;
            adw_action_row_set_subtitle(ADW_ACTION_ROW(st->rom_rows[r]),
                                        have ? "Imported" : "Not imported");
            gtk_widget_set_sensitive(st->rom_remove[r], have);
        }
    st->syncing = FALSE;
}

/* ---- the BIOS slots ---------------------------------------------------------------- */

static void on_slot_imported(gpointer ud)
{
    PrefsState *st = ud;
    /* the BIOS choice gains the imported image; the console picks it up
     * when Preferences closes */
    st->pending = MAX(st->pending, A52_APPLY_POWER);
    refresh_rows(st);
}

static void on_slot_import(GtkButton *b, gpointer ud)
{
    PrefsState *st = ud;
    int slot = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "a52-rom"));
    a52_window_import_bios(st->window, 0, slot, on_slot_imported, st);
}

static void on_slot_remove(GtkButton *b, gpointer ud)
{
    PrefsState *st = ud;
    int slot = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "a52-rom"));
    if (a52session_rom_remove(st->session, slot) == 0) {
        a52_window_toast(st->window, "BIOS removed: the built-in OS takes over");
        st->pending = MAX(st->pending, A52_APPLY_POWER);
    }
    refresh_rows(st);
}

static void on_bulk_import(GtkButton *b, gpointer ud)
{
    PrefsState *st = ud;
    int kind = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "a52-kind"));
    a52_window_import_bios(st->window, kind, -1, on_slot_imported, st);
}

static void add_bios_groups(PrefsState *st, AdwPreferencesPage *page)
{
    AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    GtkWidget *row, *box;
    static const char *const labels[] = { "Files\xe2\x80\xa6", "ZIP\xe2\x80\xa6",
                                          "Folder\xe2\x80\xa6" };
    char desc[600];

    adw_preferences_group_set_title(group, "Import BIOS");
    g_snprintf(desc, sizeof desc,
        "No BIOS is needed: the built-in Altirra 5200 OS runs everything. To use Atari's, "
        "import it from files, a ZIP (MAME's a5200.zip works) or a folder searched "
        "recursively; every recognised image is taken, the rest ignored. Kept in %s.",
        a52session_roms_path(st->session));
    adw_preferences_group_set_description(group, desc);
    row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "Import from");
    box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    for (int k = 0; k < 3; k++) {
        GtkWidget *b = gtk_button_new_with_label(labels[k]);
        g_object_set_data(G_OBJECT(b), "a52-kind", GINT_TO_POINTER(k));
        g_signal_connect(b, "clicked", G_CALLBACK(on_bulk_import), st);
        gtk_box_append(GTK_BOX(box), b);
    }
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), box);
    adw_preferences_group_add(group, row);
    adw_preferences_page_add(page, group);

    group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(group, "BIOS images");
    for (int r = 0; r < a52session_rom_slot_count() && r < MAX_ROM_ROWS; r++) {
        const a52_rom_slot *slot = a52session_rom_slot(r);
        GtkWidget *imp, *rem;
        char tip[64];
        row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), slot->name);
        imp = gtk_button_new_with_label("Import\xe2\x80\xa6");
        gtk_widget_set_valign(imp, GTK_ALIGN_CENTER);
        g_snprintf(tip, sizeof tip, "%u bytes", slot->size);
        gtk_widget_set_tooltip_text(imp, tip);
        g_object_set_data(G_OBJECT(imp), "a52-rom", GINT_TO_POINTER(r));
        g_signal_connect(imp, "clicked", G_CALLBACK(on_slot_import), st);
        rem = gtk_button_new_with_label("Remove");
        gtk_widget_set_valign(rem, GTK_ALIGN_CENTER);
        g_object_set_data(G_OBJECT(rem), "a52-rom", GINT_TO_POINTER(r));
        g_signal_connect(rem, "clicked", G_CALLBACK(on_slot_remove), st);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), imp);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), rem);
        adw_preferences_group_add(group, row);
        st->rom_rows[r] = row;
        st->rom_remove[r] = rem;
    }
    adw_preferences_page_add(page, group);
}

/* ---- the dialog --------------------------------------------------------------------- */

static void on_closed(AdwDialog *dlg, gpointer ud)
{
    PrefsState *st = ud;
    int did;
    (void)dlg;
    a52session_settings_flush(st->session);
    did = st->pending > A52_APPLY_LIVE ? a52session_apply_settings(st->session) : 0;
    a52_window_settings_changed(st->window);
    if (did == A52_APPLY_POWER)
        a52_window_toast(st->window, "Power cycled with the new configuration");
    else if (did == A52_APPLY_RESTART)
        a52_window_toast(st->window, "Restarted with the new options");
    for (int i = 0; i < MAX_ROWS; i++)
        g_free(st->labels[i]);
    g_free(st);
}

void a52_prefs_show(A52Window *parent, a52session *session)
{
    PrefsState *st = g_new0(PrefsState, 1);
    AdwPreferencesDialog *dlg = ADW_PREFERENCES_DIALOG(adw_preferences_dialog_new());
    AdwPreferencesPage *page = NULL;
    AdwPreferencesGroup *group = NULL;
    const char *last_page = NULL, *last_group = NULL;

    st->window = parent;
    st->session = session;
    st->dialog = ADW_DIALOG(dlg);
    adw_dialog_set_title(ADW_DIALOG(dlg), "Preferences");
    adw_preferences_dialog_set_search_enabled(dlg, TRUE);

    for (int i = 0; i < a52session_pref_count() && i < MAX_ROWS; i++) {
        const a52_pref *p = a52session_pref(i);
        if (!last_page || strcmp(last_page, p->page) != 0) {
            page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
            adw_preferences_page_set_title(page, p->page);
            adw_preferences_page_set_name(page, p->page);
            adw_preferences_page_set_icon_name(page, page_icon(p->page));
            adw_preferences_dialog_add(dlg, page);
            last_page = p->page;
            last_group = NULL;
        }
        if (!last_group || strcmp(last_group, p->group) != 0) {
            group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
            adw_preferences_group_set_title(group, p->group);
            if (!last_group) {
                /* the first group on a page carries the page's Help button */
                char ctx[64];
                g_snprintf(ctx, sizeof ctx, "prefs/%s", p->page);
                adw_preferences_group_set_header_suffix(group, a52_help_button(ctx));
            }
            if (!strcmp(p->group, "Console") || !strcmp(p->group, "Console BIOS"))
                adw_preferences_group_set_description(group,
                    "Console settings apply together when Preferences closes: the 5200 "
                    "power-cycles once.");
            adw_preferences_page_add(page, group);
            last_group = p->group;
        }
        st->rows[i] = make_row(st, i);
        adw_preferences_group_add(group, st->rows[i]);
        if (!strcmp(p->page, "BIOS") &&
            (i + 1 >= a52session_pref_count() || strcmp(a52session_pref(i + 1)->page, "BIOS") != 0))
            add_bios_groups(st, page);
    }
    refresh_rows(st);
    /* A52_SETTINGS_PAGE=BIOS opens on that page (screenshots) */
    if (g_getenv("A52_SETTINGS_PAGE") && *g_getenv("A52_SETTINGS_PAGE"))
        adw_preferences_dialog_set_visible_page_name(dlg, g_getenv("A52_SETTINGS_PAGE"));

    g_signal_connect(dlg, "closed", G_CALLBACK(on_closed), st);
    adw_dialog_present(ADW_DIALOG(dlg), GTK_WIDGET(parent));
}
