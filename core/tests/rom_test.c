/* BIOS import: recognised by size + CRC-32 only. Real BIOS bytes are never
   used: synthesized 2048-byte images get their CRCs registered through
   roms_test_set_crc. File, zip, folder (recursive), per-slot, probe,
   remove. */
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "a52session.h"
#include "test_tmpdir.h"

uint32_t roms_crc32(const uint8_t *p, size_t n);
void roms_test_set_crc(int slot, uint32_t crc);

#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); fflush(stderr); _exit(1); } } while (0)

static void write_file(const char *path, const uint8_t *p, size_t n)
{
    FILE *fp = fopen(path, "wb");
    CHECK(fp && fwrite(p, 1, n, fp) == n, "write %s", path);
    fclose(fp);
}

static void put16(uint8_t *p, unsigned v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v & 0xffff); put16(p + 2, v >> 16); }

/* A zip of stored entries. */
static void write_zip(const char *path, int n, const char **names, const uint8_t **d, const uint32_t *len)
{
    FILE *fp = fopen(path, "wb");
    uint8_t eo[22], ch[46], lh[30];
    uint32_t off = 0, cdsize = 0;
    int i;
    CHECK(fp, "zip %s", path);
    for (i = 0; i < n; i++) {
        uint32_t crc = roms_crc32(d[i], len[i]), nl = (uint32_t)strlen(names[i]);
        memset(lh, 0, sizeof lh);
        put32(lh, 0x04034b50); put16(lh + 4, 10); put32(lh + 14, crc);
        put32(lh + 18, len[i]); put32(lh + 22, len[i]); put16(lh + 26, nl);
        fwrite(lh, 1, 30, fp); fwrite(names[i], 1, nl, fp); fwrite(d[i], 1, len[i], fp);
        off += 30 + nl + len[i];
    }
    for (i = 0, off = 0; i < n; i++) {
        uint32_t crc = roms_crc32(d[i], len[i]), nl = (uint32_t)strlen(names[i]);
        memset(ch, 0, sizeof ch);
        put32(ch, 0x02014b50); put16(ch + 4, 20); put16(ch + 6, 10); put32(ch + 16, crc);
        put32(ch + 20, len[i]); put32(ch + 24, len[i]); put16(ch + 28, nl); put32(ch + 42, off);
        fwrite(ch, 1, 46, fp); fwrite(names[i], 1, nl, fp);
        cdsize += 46 + nl;
        off += 30 + nl + len[i];
    }
    memset(eo, 0, sizeof eo);
    put32(eo, 0x06054b50); put16(eo + 8, (unsigned)n); put16(eo + 10, (unsigned)n);
    put32(eo + 12, cdsize); put32(eo + 16, off);
    fwrite(eo, 1, 22, fp);
    fclose(fp);
}

static void mk(const char *p) { test_mkdir(p); }

static void fill(uint8_t *p, size_t n, unsigned seed)
{
    size_t i;
    for (i = 0; i < n; i++)
        p[i] = (uint8_t)((i * 31 + seed * 17 + (i >> 5)) & 0xff);
}

int main(void)
{
    char base[512], cfg[600], data[600], d1[700], d2[700], d3[700], path[900], msg[512];
    a52session_paths pp;
    a52session *s;
    static uint8_t four[2048], two[2048], junk[100], big[4096];
    int slots[8], n;
    const char *names[2];
    const uint8_t *ds[2];
    uint32_t ls[2];

    test_tmpdir(base, sizeof base, "roms");
    snprintf(cfg, sizeof cfg, "%s/config", base);
    snprintf(data, sizeof data, "%s/data", base);
    memset(&pp, 0, sizeof pp);
    pp.config_dir = cfg;
    pp.data_dir = data;
    pp.fujinet_lib = "";
    s = a52session_new(&pp);
    CHECK(s, "new");
    CHECK(a52session_rom_slot_count() == 2, "two slots");
    CHECK(a52session_rom_slot(0)->size == 2048 && a52session_rom_slot(1)->size == 2048, "sizes");

    fill(four, sizeof four, 1);
    fill(two, sizeof two, 2);
    fill(junk, sizeof junk, 3);
    fill(big, sizeof big, 4);
    roms_test_set_crc(0, roms_crc32(four, sizeof four));
    roms_test_set_crc(1, roms_crc32(two, sizeof two));
    CHECK(!a52session_rom_available(s, 0) && !a52session_rom_available(s, 1), "none yet");

    /* a plain file, any name */
    snprintf(d1, sizeof d1, "%s/co19156.u8", base);
    write_file(d1, four, sizeof four);
    n = a52session_rom_probe(s, d1, slots, 8);
    CHECK(n == 1 && slots[0] == 0, "probe 4-port: %d", n);
    CHECK(a52session_rom_import(s, d1, -1, msg, sizeof msg) == 1, "import: %s", msg);
    CHECK(strstr(msg, "4-port"), "msg %s", msg);
    CHECK(a52session_rom_available(s, 0) && !a52session_rom_available(s, 1), "4-port only");
    CHECK(a52session_rom_remove(s, 0) == 0 && !a52session_rom_available(s, 0), "remove");
    CHECK(a52session_rom_import(s, d1, -1, msg, sizeof msg) == 1, "re-import: %s", msg);

    /* wrong size / unknown: refused with a message */
    snprintf(d2, sizeof d2, "%s/junk.bin", base);
    write_file(d2, junk, sizeof junk);
    msg[0] = 0;
    CHECK(a52session_rom_import(s, d2, -1, msg, sizeof msg) < 0 && msg[0], "junk refused: %s", msg);
    CHECK(a52session_rom_probe(s, d2, slots, 8) == 0, "junk probe");
    msg[0] = 0;
    CHECK(a52session_rom_import(s, d2, 1, msg, sizeof msg) < 0 && strstr(msg, "size"), "size msg: %s", msg);
    snprintf(d2, sizeof d2, "%s/big.bin", base);
    write_file(d2, big, sizeof big);
    CHECK(a52session_rom_import(s, d2, -1, msg, sizeof msg) < 0, "4K refused");

    /* per-slot: the right file in, the wrong one out (and it says what it is) */
    snprintf(d2, sizeof d2, "%s/co19156a.a8", base);
    write_file(d2, two, sizeof two);
    msg[0] = 0;
    CHECK(a52session_rom_import(s, d2, 0, msg, sizeof msg) < 0 && strstr(msg, "2-port"), "wrong slot: %s", msg);
    CHECK(!a52session_rom_available(s, 1), "2-port untouched");
    CHECK(a52session_rom_import(s, d2, 1, msg, sizeof msg) == 1, "slot 1: %s", msg);
    CHECK(a52session_rom_available(s, 1), "2-port in");
    a52session_rom_remove(s, 0);
    a52session_rom_remove(s, 1);

    /* a zip with both and an unrelated file (MAME names) */
    names[0] = "co19156.u8"; ds[0] = four; ls[0] = sizeof four;
    names[1] = "readme.txt"; ds[1] = junk; ls[1] = sizeof junk;
    snprintf(d3, sizeof d3, "%s/a5200.zip", base);
    write_zip(d3, 2, names, ds, ls);
    n = a52session_rom_probe(s, d3, slots, 8);
    CHECK(n == 1 && slots[0] == 0, "zip probe %d", n);
    CHECK(a52session_rom_import(s, d3, -1, msg, sizeof msg) == 1, "zip: %s", msg);
    CHECK(strstr(msg, "ignored 1"), "zip msg: %s", msg);
    CHECK(a52session_rom_available(s, 0) && !a52session_rom_available(s, 1), "zip -> 4-port");
    a52session_rom_remove(s, 0);

    /* a folder, nested, with strangers */
    snprintf(path, sizeof path, "%s/folder", base); mk(path);
    snprintf(path, sizeof path, "%s/folder/a", base); mk(path);
    snprintf(path, sizeof path, "%s/folder/a/b", base); mk(path);
    snprintf(path, sizeof path, "%s/folder/a/b/x.bin", base); write_file(path, two, sizeof two);
    snprintf(path, sizeof path, "%s/folder/y.rom", base); write_file(path, four, sizeof four);
    snprintf(path, sizeof path, "%s/folder/a/notes.txt", base); write_file(path, junk, sizeof junk);
    snprintf(path, sizeof path, "%s/folder/z.bin", base); write_file(path, big, sizeof big);
    snprintf(path, sizeof path, "%s/folder", base);
    n = a52session_rom_probe(s, path, slots, 8);
    CHECK(n == 2, "folder probe %d", n);
    CHECK(a52session_rom_import(s, path, -1, msg, sizeof msg) == 2, "folder: %s", msg);
    CHECK(strstr(msg, "ignored 2"), "folder msg: %s", msg);
    CHECK(a52session_rom_available(s, 0) && a52session_rom_available(s, 1), "folder -> both");
    CHECK(roms_crc32((const uint8_t *)"", 0) == 0, "crc of nothing");

    /* settings: the path lands where the session keeps ROMs */
    CHECK(strstr(a52session_roms_path(s), "roms"), "roms path %s", a52session_roms_path(s));

    /* an empty folder, and a missing path: -1 with a message */
    snprintf(path, sizeof path, "%s/empty", base); mk(path);
    msg[0] = 0;
    CHECK(a52session_rom_import(s, path, -1, msg, sizeof msg) < 0 && msg[0], "empty folder: %s", msg);
    snprintf(path, sizeof path, "%s/nonexistent", base);
    msg[0] = 0;
    CHECK(a52session_rom_import(s, path, -1, msg, sizeof msg) < 0 && msg[0], "missing: %s", msg);

    a52session_free(s);
    puts("roms: ok");
    return 0;
}
