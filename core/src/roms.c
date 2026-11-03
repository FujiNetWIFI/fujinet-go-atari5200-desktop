/*
 * roms.c -- the BIOS slots, identifying and importing a user's own images.
 *
 * The Atari 5200 has two real BIOS revisions, both 2048 bytes: the 4-port
 * (CRC-32 4248d3e3, MAME co19156.u8) and the 2-port (c2ba2613, co19156a.a8).
 * Imports are recognised by size and CRC-32 only (any file name), from a
 * single file, a ZIP (MAME's a5200.zip / a5200a.zip work) or a FOLDER searched
 * recursively; every recognised image is taken and the rest ignored. They are
 * copied to <data>/roms/<slot key>.rom and a slot is available when its file
 * exists. The built-in Altirra 5200 OS is not a slot: it is the fallback.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "roms_embedded.h"
#include "session_internal.h"
#include "zipread.h"

typedef struct {
    a52_rom_slot pub;
    const char *config_key;     /* atari800's: it picks the type by CRC */
    uint32_t crc;
} slot_info;

#define K *1024u

static slot_info s_slots[ROM_COUNT] = {
    {{"bios_4port", "Atari 5200 BIOS (4-port)", "Console BIOS", 2 K}, "5200_ROM", 0x4248d3e3},
    {{"bios_2port", "Atari 5200 BIOS (2-port)", "Console BIOS", 2 K}, "5200_ROM", 0xc2ba2613},
};

/* Tests only: the real CRCs cannot be matched by synthesized images. */
void roms_test_set_crc(int slot, uint32_t crc)
{
    if (slot >= 0 && slot < ROM_COUNT)
        s_slots[slot].crc = crc;
}

int a52session_rom_slot_count(void)
{
    return ROM_COUNT;
}

const a52_rom_slot *a52session_rom_slot(int i)
{
    return i >= 0 && i < ROM_COUNT ? &s_slots[i].pub : NULL;
}

const char *roms_config_key(int slot)
{
    return slot >= 0 && slot < ROM_COUNT ? s_slots[slot].config_key : NULL;
}

static void slot_file(struct a52session *s, int slot, char *dst, size_t n)
{
    snprintf(dst, n, "%.900s/%s.rom", s->roms_dir, s_slots[slot].pub.key);
}

static int file_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static int dir_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

int a52session_rom_available(a52session *s, int slot)
{
    char p[A52_PATH_MAX];
    if (slot < 0 || slot >= ROM_COUNT)
        return 0;
    slot_file(s, slot, p, sizeof p);
    return file_exists(p);
}

/* The imported file, or NULL -- a static buffer per slot. */
const char *roms_path(struct a52session *s, int slot)
{
    static char paths[ROM_COUNT][A52_PATH_MAX];
    if (slot < 0 || slot >= ROM_COUNT)
        return NULL;
    slot_file(s, slot, paths[slot], sizeof paths[slot]);
    if (!file_exists(paths[slot]))
        return NULL;
    return paths[slot];
}

/* ---- identification ------------------------------------------------------- */

uint32_t roms_crc32(const uint8_t *p, size_t n)
{
    static uint32_t table[256];
    static int init;
    uint32_t c = 0xffffffffu;
    if (!init) {
        uint32_t i, j, v;
        for (i = 0; i < 256; i++) {
            v = i;
            for (j = 0; j < 8; j++)
                v = v & 1 ? 0xedb88320u ^ (v >> 1) : v >> 1;
            table[i] = v;
        }
        init = 1;
    }
    while (n--)
        c = table[(c ^ *p++) & 0xff] ^ (c >> 8);
    return ~c;
}

static int slot_for_crc(uint32_t size, uint32_t crc)
{
    int i;
    for (i = 0; i < ROM_COUNT; i++)
        if (s_slots[i].crc == crc && s_slots[i].pub.size == size)
            return i;
    return -1;
}

typedef struct {
    uint8_t *data;
    uint32_t size;
    uint32_t crc;
    char name[160];             /* for messages */
} blob;

typedef struct {
    blob *b;
    int n, cap;
    int unreadable;
} blobset;

static int write_slot(struct a52session *s, int slot, const uint8_t *data, uint32_t size)
{
    char p[A52_PATH_MAX], tmp[A52_PATH_MAX + 8];
    FILE *fp;
    paths_mkdir_p(s->roms_dir);
    slot_file(s, slot, p, sizeof p);
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    fp = fopen(tmp, "wb");
    if (!fp)
        return -1;
    if (fwrite(data, 1, size, fp) != size) {
        fclose(fp);
        remove(tmp);
        return -1;
    }
    fclose(fp);
#if defined(_WIN32)
    remove(p);
#endif
    return rename(tmp, p);
}

static int read_file(const char *path, uint8_t **data, uint32_t *size)
{
    FILE *fp = fopen(path, "rb");
    long n;
    if (!fp)
        return -1;
    fseek(fp, 0, SEEK_END);
    n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (n <= 0 || n > 4 * 1024 * 1024) {
        fclose(fp);
        return -1;
    }
    *data = malloc((size_t)n);
    if (!*data || fread(*data, 1, (size_t)n, fp) != (size_t)n) {
        free(*data);
        *data = NULL;
        fclose(fp);
        return -1;
    }
    fclose(fp);
    *size = (uint32_t)n;
    return 0;
}

static const char *base_name(const char *p)
{
    const char *q = p, *r;
    for (r = p; *r; r++)
        if (*r == '/' || *r == '\\')
            q = r + 1;
    return q;
}

static void add_blob(blobset *bs, const char *name, uint8_t *d, uint32_t sz)
{
    blob *b;
    if (bs->n == bs->cap) {
        int nc = bs->cap ? bs->cap * 2 : 8;
        blob *nb = realloc(bs->b, (size_t)nc * sizeof *nb);
        if (!nb) {
            free(d);
            return;
        }
        bs->b = nb;
        bs->cap = nc;
    }
    b = &bs->b[bs->n++];
    b->data = d;
    b->size = sz;
    b->crc = roms_crc32(d, sz);
    snprintf(b->name, sizeof b->name, "%s", base_name(name));
}

/* A zip's entries, or the file itself. */
static void load_file_blobs(const char *path, blobset *bs)
{
    zipread *z = zipread_open(path);
    if (z) {
        int i, e = zipread_count(z);
        for (i = 0; i < e; i++) {
            uint8_t *d;
            uint32_t sz;
            if (zipread_extract(z, i, &d, &sz, 4 * 1024 * 1024) == 0)
                add_blob(bs, zipread_name(z, i), d, sz);
            else
                bs->unreadable++;
        }
        zipread_close(z);
    }
    else {
        uint8_t *d;
        uint32_t sz;
        if (read_file(path, &d, &sz) == 0)
            add_blob(bs, path, d, sz);
        else
            bs->unreadable++;
    }
}

static void load_dir_blobs(const char *dir, blobset *bs, int depth)
{
    DIR *dp = opendir(dir);
    struct dirent *de;
    if (!dp)
        return;
    while ((de = readdir(dp)) != NULL) {
        char p[A52_PATH_MAX];
        struct stat st;
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        snprintf(p, sizeof p, "%.900s/%.100s", dir, de->d_name);
        if (stat(p, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 16)
                load_dir_blobs(p, bs, depth + 1);
        }
        else if (S_ISREG(st.st_mode) && st.st_size <= 4 * 1024 * 1024)
            load_file_blobs(p, bs);
    }
    closedir(dp);
}

static int load_blobs(const char *path, blobset *bs)
{
    memset(bs, 0, sizeof *bs);
    if (dir_exists(path))
        load_dir_blobs(path, bs, 0);
    else
        load_file_blobs(path, bs);
    return bs->n > 0 ? 0 : -1;
}

static void free_blobs(blobset *bs)
{
    int i;
    for (i = 0; i < bs->n; i++)
        free(bs->b[i].data);
    free(bs->b);
}

static void append_msg(char *msg, int msgsz, const char *what)
{
    size_t l;
    if (!msg || msgsz <= 0)
        return;
    l = strlen(msg);
    snprintf(msg + l, (size_t)msgsz - l, "%s%s", l ? ", " : "", what);
}

int roms_identify_file(const char *path)
{
    blobset bs;
    int i, hit = 0;
    if (load_blobs(path, &bs) != 0)
        return 0;
    for (i = 0; i < bs.n && !hit; i++)
        hit = slot_for_crc(bs.b[i].size, bs.b[i].crc) >= 0;
    free_blobs(&bs);
    return hit;
}

int a52session_rom_probe(a52session *s, const char *path, int *slots, int max)
{
    blobset bs;
    int i, k = 0, slot, j;
    (void)s;
    if (load_blobs(path, &bs) != 0)
        return 0;
    for (i = 0; i < bs.n; i++) {
        slot = slot_for_crc(bs.b[i].size, bs.b[i].crc);
        for (j = 0; slot >= 0 && j < k; j++)
            if (slots[j] == slot)
                slot = -1;
        if (slot >= 0 && k < max)
            slots[k++] = slot;
    }
    free_blobs(&bs);
    return k;
}

int a52session_rom_import(a52session *s, const char *path, int slot, char *msg, int msgsz)
{
    blobset bs;
    int i, imported = 0, ignored = 0, done[ROM_COUNT] = {0};
    char tmp[256];

    if (msg && msgsz > 0)
        msg[0] = 0;
    if (slot >= ROM_COUNT) {
        if (msg)
            snprintf(msg, (size_t)msgsz, "No such BIOS slot");
        return -1;
    }
    if (load_blobs(path, &bs) != 0) {
        if (msg)
            snprintf(msg, (size_t)msgsz, dir_exists(path) ? "No files found in %s"
                                                          : "Could not read %s", path);
        return -1;
    }
    if (slot < 0) {
        for (i = 0; i < bs.n; i++) {
            int sl = slot_for_crc(bs.b[i].size, bs.b[i].crc);
            if (sl >= 0 && !done[sl] && write_slot(s, sl, bs.b[i].data, bs.b[i].size) == 0) {
                done[sl] = 1;
                append_msg(msg, msgsz, s_slots[sl].pub.name);
                imported++;
            }
            else if (sl < 0)
                ignored++;
        }
        if (!imported) {
            if (msg)
                snprintf(msg, (size_t)msgsz, "Not an Atari 5200 BIOS this app recognises (%d file%s checked)",
                         bs.n, bs.n == 1 ? "" : "s");
        }
        else if (ignored && msg) {
            snprintf(tmp, sizeof tmp, "imported; ignored %d unrecognised file%s", ignored,
                     ignored == 1 ? "" : "s");
            append_msg(msg, msgsz, tmp);
        }
    }
    else {
        /* one slot: the file (or the one matching entry) must be that image */
        int found = -1, other = -1;
        for (i = 0; i < bs.n; i++) {
            int sl = slot_for_crc(bs.b[i].size, bs.b[i].crc);
            if (sl == slot)
                found = i;
            else if (sl >= 0)
                other = sl;
        }
        if (found >= 0 && write_slot(s, slot, bs.b[found].data, bs.b[found].size) == 0) {
            imported = 1;
            append_msg(msg, msgsz, s_slots[slot].pub.name);
        }
        else if (found >= 0) {
            if (msg)
                snprintf(msg, (size_t)msgsz, "Could not write to %s", s->roms_dir);
        }
        else if (msg) {
            if (other >= 0)
                snprintf(msg, (size_t)msgsz, "That file is the %s, not the %s",
                         s_slots[other].pub.name, s_slots[slot].pub.name);
            else if (bs.n == 1 && bs.b[0].size != s_slots[slot].pub.size)
                snprintf(msg, (size_t)msgsz, "That file is not the size a %s is (%u bytes, not %u)",
                         s_slots[slot].pub.name, (unsigned)bs.b[0].size, (unsigned)s_slots[slot].pub.size);
            else
                snprintf(msg, (size_t)msgsz, "That is not a %s", s_slots[slot].pub.name);
        }
    }
    free_blobs(&bs);
    if (imported)
        session_roms_changed(s);
    return imported > 0 ? imported : -1;
}

int a52session_rom_remove(a52session *s, int slot)
{
    char p[A52_PATH_MAX];
    if (slot < 0 || slot >= ROM_COUNT)
        return -1;
    slot_file(s, slot, p, sizeof p);
    if (file_exists(p) && remove(p) != 0)
        return -1;
    session_roms_changed(s);
    return 0;
}

/* A developer build's embedded images, written out once (WITH_A5200_ROMS). */
void roms_provision_embedded(struct a52session *s)
{
    unsigned i;
    int slot;
    for (i = 0; i < a52_embedded_rom_count; i++) {
        for (slot = 0; slot < ROM_COUNT; slot++)
            if (!strcmp(s_slots[slot].pub.key, a52_embedded_roms[i].key))
                break;
        if (slot < ROM_COUNT && !a52session_rom_available(s, slot))
            write_slot(s, slot, a52_embedded_roms[i].data, a52_embedded_roms[i].size);
    }
}
