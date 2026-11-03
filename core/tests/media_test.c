/* Opening things: a game from a file, from a zip (one entry; MAME style), a
   .car; the cartridge planner's verdicts; routing (BIOS vs cartridge vs the SD
   folder); a BIOS dropped as a file, zip and folder is imported (a synthesized
   image through the test CRC hook), not run. No ROM. */
#include "test_util.h"
#include "test_files.h"

extern void roms_test_set_crc(int slot, uint32_t crc);
extern uint32_t roms_crc32(const uint8_t *p, size_t n);

/* A stored (method 0) zip of one named entry. */
static void write_zip(const char *path, const char *name, const uint8_t *d, uint32_t n)
{
    FILE *fp = fopen(path, "wb");
    uint32_t crc = roms_crc32(d, n), off = 0, cdsz;
    uint16_t nl = (uint16_t)strlen(name);
    uint8_t h[46];
#define W16(p, o, v) ((p)[o] = (uint8_t)(v), (p)[(o) + 1] = (uint8_t)((v) >> 8))
#define W32(p, o, v) (W16(p, o, v), W16(p, (o) + 2, (v) >> 16))
    memset(h, 0, sizeof h);
    W32(h, 0, 0x04034b50u); W16(h, 4, 20); W32(h, 14, crc); W32(h, 18, n); W32(h, 22, n); W16(h, 26, nl);
    fwrite(h, 1, 30, fp); fwrite(name, 1, nl, fp); fwrite(d, 1, n, fp);
    memset(h, 0, sizeof h);
    W32(h, 0, 0x02014b50u); W16(h, 4, 20); W16(h, 6, 20); W32(h, 16, crc); W32(h, 20, n); W32(h, 24, n);
    W16(h, 28, nl); W32(h, 42, off);
    fwrite(h, 1, 46, fp); fwrite(name, 1, nl, fp);
    cdsz = 46 + nl;
    memset(h, 0, sizeof h);
    W32(h, 0, 0x06054b50u); W16(h, 8, 1); W16(h, 10, 1); W32(h, 12, cdsz); W32(h, 16, 30 + nl + n);
    fwrite(h, 1, 22, fp);
    fclose(fp);
}

int main(void)
{
    a52session *s = test_session("media");
    char base[512], path[800], path2[800], msg[300], why[200], dest[900];
    static uint8_t img[0x8000], bios[2048];
    FILE *fp;
    int i;

    CHECK(s, "session");
    test_tmpdir(base, sizeof base, "media-files");
    test_start(s, 0);
    CHECK(test_wait_frames(s, 20), "runs");

    /* planner verdicts without starting anything */
    snprintf(path, sizeof path, "%s/a.a52", base);
    write_game(path, 0x2000, 0);
    CHECK(a52session_check_cart(path, why, sizeof why), "8K ok: %s", why);
    CHECK(!a52session_cart_is_app(path), "not an app");
    write_game(path, 0x8000, 1);
    CHECK(a52session_cart_is_app(path), "claimed app");
    snprintf(path2, sizeof path2, "%s/empty.bin", base);
    fp = fopen(path2, "wb"); fclose(fp);
    CHECK(!a52session_check_cart(path2, why, sizeof why) && why[0], "empty refused: %s", why);
    CHECK(a52session_media_kind(s, path2) == A52_MEDIA_UNKNOWN, "empty is unknown");
    CHECK(a52session_media_kind(s, path) == A52_MEDIA_CARTRIDGE, "cartridge");
    CHECK(a52session_media_kind(s, base) == A52_MEDIA_UNKNOWN, "a folder of carts is not a BIOS");

    /* a game from a zip, MAME style */
    make_game(img, 0x2000, 0);
    snprintf(path, sizeof path, "%s/zipped.zip", base);
    write_zip(path, "game.bin", img, 0x2000);
    CHECK(a52session_media_kind(s, path) == A52_MEDIA_CARTRIDGE, "zip is a cartridge");
    CHECK(a52session_open(s, path, msg, sizeof msg) == 0, "open zip: %s", msg);
    CHECK(test_wait_byte(s, 0x0400, 0xAA, 30000), "the zipped game runs");
    CHECK(strcmp(a52session_cart_path(s), path) == 0, "path");

    /* a .car */
    snprintf(path, sizeof path, "%s/g.car", base);
    write_game_car(path);
    CHECK(a52session_open(s, path, msg, sizeof msg) == 0, "open car: %s", msg);

    /* BIOS: file, zip, folder; an imported BIOS is not run */
    for (i = 0; i < 2048; i++)
        bios[i] = (uint8_t)(i * 5 + 1);
    roms_test_set_crc(0, roms_crc32(bios, sizeof bios));
    snprintf(path, sizeof path, "%s/bios.rom", base);
    fp = fopen(path, "wb"); fwrite(bios, 1, sizeof bios, fp); fclose(fp);
    CHECK(a52session_media_kind(s, path) == A52_MEDIA_BIOS, "BIOS file");
    CHECK(a52session_open_or_import(s, path, msg, sizeof msg) == A52_MEDIA_BIOS, "import: %s", msg);
    CHECK(strstr(msg, "4-port"), "message: %s", msg);
    CHECK(a52session_rom_available(s, 0), "available");
    CHECK(a52session_rom_remove(s, 0) == 0 || !a52session_rom_available(s, 0), "removed");
    CHECK(!a52session_rom_available(s, 0), "gone");
    snprintf(path, sizeof path, "%s/bios.zip", base);
    write_zip(path, "co19156.u8", bios, sizeof bios);
    CHECK(a52session_media_kind(s, path) == A52_MEDIA_BIOS, "BIOS zip");
    CHECK(a52session_open(s, path, msg, sizeof msg) == 0 && a52session_rom_available(s, 0), "zip import: %s", msg);
    a52session_rom_remove(s, 0);

    /* everything else goes to the SD folder (archives of one cart unpacked) */
    snprintf(path, sizeof path, "%s/notes.txt", base);
    fp = fopen(path, "wb"); fputs("hello", fp); fclose(fp);
    CHECK(a52session_open_or_import(s, path, msg, sizeof msg) == 0, "to SD: %s", msg);
    snprintf(path2, sizeof path2, "%s/notes.txt", a52session_sd_path(s));
    fp = fopen(path2, "rb"); CHECK(fp != NULL, "copied"); if (fp) fclose(fp);
    snprintf(path, sizeof path, "%s/c.bin", base);
    write_game(path, 0x2000, 0);
    CHECK(a52session_import_to_sd(s, path, dest, sizeof dest) == 0 && strstr(dest, "c.a52"), "bin becomes .a52: %s", dest);

    /* eject = power cycle to CONFIG */
    CHECK(a52session_eject(s) == 0, "eject");
    CHECK(test_wait_config_text(s, "HOST SLOTS", 30000), "CONFIG");
    CHECK(a52session_cart_path(s)[0] == 0, "no cart");

    a52session_stop(s);
    a52session_free(s);
    puts("media: ok");
    return 0;
}
