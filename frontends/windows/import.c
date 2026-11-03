/*
 * Choosing files: cartridges for Open and the SD import, and the console BIOS
 * (files, a ZIP or a folder), each path through the session, then one message
 * box saying what came of it.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "import.h"

#include <commdlg.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>

#include "win_common.h"

static const char k_cart_filter[] =
    "Cartridge images (*.a52;*.bin;*.rom;*.car;*.zip)\0*.a52;*.bin;*.rom;*.car;*.zip\0All files\0*.*\0\0";
static const char k_bios_filter[] =
    "BIOS images (*.rom;*.bin;*.u8;*.a8;*.zip)\0*.rom;*.bin;*.u8;*.a8;*.zip\0All files\0*.*\0\0";
static const char k_zip_filter[] = "ZIP archives (*.zip)\0*.zip\0All files\0*.*\0\0";

/* A GetOpenFileName multi-select buffer is "dir\0name\0name\0\0", or one full
 * path when a single file was chosen. Calls fn for each full path; 0 when the
 * chooser was cancelled. */
static int pick_each(HWND owner, const char *filter, const char *title,
                     void (*fn)(const char *path, void *ctx), void *ctx)
{
    static char buf[64 * 1024];
    OPENFILENAMEA ofn;
    const char *p;
    memset(&ofn, 0, sizeof ofn);
    buf[0] = buf[1] = '\0';
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = sizeof buf;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER |
                OFN_ALLOWMULTISELECT;
    if (!GetOpenFileNameA(&ofn))
        return 0;
    p = buf + strlen(buf) + 1;
    if (!*p) {
        fn(buf, ctx);
        return 1;
    }
    for (; *p; p += strlen(p) + 1) {
        char path[2048];
        snprintf(path, sizeof path, "%.1000s\\%.1000s", buf, p);
        fn(path, ctx);
    }
    return 1;
}

static int pick_one(HWND owner, const char *filter, const char *title, char *path, DWORD pathsz)
{
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof ofn);
    path[0] = '\0';
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = pathsz;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameA(&ofn) ? 1 : 0;
}

static int pick_folder(HWND owner, const char *title, char *path)
{
    BROWSEINFOA bi;
    PIDLIST_ABSOLUTE pidl;
    memset(&bi, 0, sizeof bi);
    bi.hwndOwner = owner;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    pidl = SHBrowseForFolderA(&bi);
    if (!pidl) return 0;
    if (!SHGetPathFromIDListA(pidl, path)) path[0] = '\0';
    CoTaskMemFree(pidl);
    return path[0] != '\0';
}

int a52_pick_cart(HWND owner, const char *title, char *path, DWORD pathsz)
{
    return pick_one(owner, k_cart_filter, title, path, pathsz);
}

/* ---- cartridges for the SD host ---------------------------------------------- */

typedef struct {
    a52session *session;
    int ok, failed;
    char first_name[MAX_PATH];
    char errors[2048];
} cart_log;

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '\\'), *t = strrchr(path, '/');
    if (t && (!s || t > s)) s = t;
    return s ? s + 1 : path;
}

static void import_cart(const char *path, void *ctx)
{
    cart_log *l = ctx;
    char dest[1024];
    dest[0] = '\0';
    if (a52session_import_to_sd(l->session, path, dest, sizeof dest) == 0) {
        if (!l->ok)
            snprintf(l->first_name, sizeof l->first_name, "%s", base_name(dest[0] ? dest : path));
        l->ok++;
    } else {
        size_t len = strlen(l->errors);
        l->failed++;
        if (len < sizeof l->errors - 1)
            snprintf(l->errors + len, sizeof l->errors - len, "%s%s", len ? "\n" : "",
                     base_name(path));
    }
}

int a52_import_carts(HWND owner, a52session *session)
{
    static cart_log l;
    char note[2400];
    memset(&l, 0, sizeof l);
    l.session = session;
    if (!pick_each(owner, "All files\0*.*\0\0", "Import to the FujiNet SD folder", import_cart, &l))
        return -1;
    if (l.failed) {
        if (l.ok)
            snprintf(note, sizeof note, "%d imported, %d failed:\n%s", l.ok, l.failed, l.errors);
        else
            snprintf(note, sizeof note, "Could not copy:\n%s", l.errors);
        a52_message(owner, note, "Import failed", MB_ICONWARNING | MB_OK);
    } else if (l.ok == 1) {
        snprintf(note, sizeof note, "%s is on the SD host. Boot it from the CONFIG client.", l.first_name);
        a52_message(owner, note, "Imported", MB_ICONINFORMATION | MB_OK);
    } else if (l.ok > 1) {
        snprintf(note, sizeof note, "%d files are on the SD host. Boot them from the CONFIG client.", l.ok);
        a52_message(owner, note, "Imported", MB_ICONINFORMATION | MB_OK);
    }
    return l.ok;
}

/* ---- BIOS ----------------------------------------------------------------------- */

typedef struct {
    a52session *session;
    int slot;
    int imported, tried;
    char msg[4096];
} bios_log;

static void add_message(bios_log *l, const char *path, int rc, const char *m)
{
    size_t len = strlen(l->msg);
    if (len >= sizeof l->msg - 1) return;
    snprintf(l->msg + len, sizeof l->msg - len, "%s%s%s%s", len ? "\n\n" : "",
             l->tried > 1 || rc < 0 ? base_name(path) : "", l->tried > 1 || rc < 0 ? ": " : "", m);
}

static void import_one(const char *path, void *ctx)
{
    bios_log *l = ctx;
    char m[1024];
    int rc;
    m[0] = '\0';
    l->tried++;
    rc = a52session_rom_import(l->session, path, l->slot, m, sizeof m);
    if (rc > 0) l->imported += rc;
    add_message(l, path, rc, m[0] ? m : rc > 0 ? "imported" : "nothing recognised");
}

void a52_show_import_message(HWND owner, int imported, const char *msg)
{
    a52_message(owner, msg && *msg ? msg : imported > 0 ? "Imported." : "Nothing was recognised.",
                imported > 0 ? "BIOS imported" : "Nothing imported",
                imported > 0 ? MB_ICONINFORMATION | MB_OK : MB_ICONWARNING | MB_OK);
}

int a52_import_bios(HWND owner, a52session *session, int how, int slot)
{
    static bios_log l;
    char path[MAX_PATH];
    memset(&l, 0, sizeof l);
    l.session = session;
    l.slot = slot;
    if (how == A52_BIOS_FILES) {
        if (!pick_each(owner, k_bios_filter, "Import BIOS: choose files", import_one, &l))
            return -1;
    } else if (how == A52_BIOS_ZIP) {
        if (!pick_one(owner, k_zip_filter, "Import BIOS: choose a ZIP (MAME's a5200.zip works)", path, sizeof path))
            return -1;
        import_one(path, &l);
    } else {
        if (!pick_folder(owner, "Import BIOS: choose a folder (searched recursively)", path))
            return -1;
        import_one(path, &l);
    }
    a52_show_import_message(owner, l.imported, l.msg);
    return l.imported;
}

int a52_import_bios_menu(HWND owner, a52session *session, int slot, int x, int y)
{
    HMENU m = CreatePopupMenu();
    int cmd;
    AppendMenuA(m, MF_STRING, 1, "Files...");
    AppendMenuA(m, MF_STRING, 2, "ZIP...");
    if (slot < 0)
        AppendMenuA(m, MF_STRING, 3, "Folder...");
    cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN, x, y, 0, owner, NULL);
    DestroyMenu(m);
    if (cmd < 1) return -1;
    return a52_import_bios(owner, session, cmd == 1 ? A52_BIOS_FILES : cmd == 2 ? A52_BIOS_ZIP : A52_BIOS_FOLDER,
                           slot);
}
