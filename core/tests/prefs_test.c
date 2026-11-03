/* Preferences: the table every frontend renders, BIOS choices (only imported
   ones selectable), apply levels, text validation, and what reaches atari800
   (the BIOS in use after a power-cycle apply). No ROM: a synthesized image is
   imported through the test CRC hook. */
#include "test_util.h"

extern void roms_test_set_crc(int slot, uint32_t crc);
extern uint32_t roms_crc32(const uint8_t *p, size_t n);

int main(void)
{
    a52session *s = test_session("prefs");
    int i, n, bios, tv;
    char name[96];
    int pages = 0;
    const char *last = "";

    CHECK(s, "session");
    n = a52session_pref_count();
    CHECK(n > 8, "prefs: %d", n);
    for (i = 0; i < n; i++) {
        const a52_pref *p = a52session_pref(i);
        CHECK(p && p->key && p->page && p->label && p->group, "pref %d complete", i);
        CHECK(a52session_pref_find(p->key) == i, "find %s", p->key);
        if (strcmp(p->page, last)) {
            pages++;
            last = p->page;
        }
        if (p->type == A52_PREF_CHOICE) {
            CHECK(a52session_pref_choice(s, i, 0) != NULL, "%s has choices", p->key);
            CHECK(a52session_pref_choice(s, i, 99) == NULL, "%s ends", p->key);
        }
    }
    CHECK(pages >= 4, "pages %d", pages);
    CHECK(a52session_pref_find("machine") == -1 && a52session_pref_find("basic") == -1 &&
          a52session_pref_find("port1_device") == -1, "no computer prefs");

    /* BIOS: built-in by default, the real ones greyed until imported */
    bios = a52session_pref_find("bios");
    CHECK(bios >= 0, "bios pref");
    CHECK(a52session_pref_get_choice(s, bios) == 0, "built-in by default");
    CHECK(a52session_pref_choice_enabled(s, bios, 0), "built-in selectable");
    CHECK(!a52session_pref_choice_enabled(s, bios, 1) && !a52session_pref_choice_enabled(s, bios, 2),
          "real BIOS greyed");
    a52session_pref_set_choice(s, bios, 1);
    CHECK(a52session_pref_get_choice(s, bios) == 0, "an imported-less choice falls back");

    /* import a synthesized image into slot 0 (the 4-port) and select it */
    {
        uint8_t img[2048];
        char dir[512], path[600], msg[256];
        FILE *fp;
        for (i = 0; i < 2048; i++)
            img[i] = (uint8_t)(i * 7 + 3);
        img[0x7FC] = 0x00; img[0x7FD] = 0xF8;
        roms_test_set_crc(0, roms_crc32(img, sizeof img));
        test_tmpdir(dir, sizeof dir, "prefs-files");
        snprintf(path, sizeof path, "%s/bios.bin", dir);
        fp = fopen(path, "wb");
        fwrite(img, 1, sizeof img, fp);
        fclose(fp);
        CHECK(a52session_rom_import(s, path, -1, msg, sizeof msg) == 1, "import: %s", msg);
        CHECK(a52session_rom_available(s, 0), "available");
        CHECK(a52session_pref_choice_enabled(s, bios, 1), "now selectable");
        a52session_pref_set_choice(s, bios, 1);
        CHECK(a52session_pref_get_choice(s, bios) == 1, "chosen");
        a52session_pref_set_choice(s, bios, 0);
    }

    /* apply levels */
    tv = a52session_pref_find("tv");
    CHECK(a52session_pref(tv)->applies == A52_APPLY_POWER, "tv needs a power cycle");
    CHECK(a52session_pref(a52session_pref_find("volume"))->applies == A52_APPLY_LIVE, "volume is live");
    CHECK(a52session_pref(a52session_pref_find("enable_fujinet"))->applies == A52_APPLY_RESTART, "fujinet restarts");

    /* running: a live change applies at once, a TV change re-initialises the core */
    test_start(s, 0);
    CHECK(test_wait_frames(s, 20), "runs");
    CHECK(a52session_refresh_rate(s) == 59923 || a52session_refresh_rate(s) == 60000 ||
              a52session_refresh_rate(s) / 1000 == 59 || a52session_refresh_rate(s) / 1000 == 60,
          "NTSC rate %d", a52session_refresh_rate(s));
    a52session_set_int(s, "volume", 30);
    CHECK(a52session_apply_settings(s) == A52_APPLY_LIVE, "volume applies live");
    a52session_set_int(s, "tv", 1);
    CHECK(a52session_apply_settings(s) == A52_APPLY_POWER, "tv applies with a power cycle");
    CHECK(test_wait_frames(s, 20), "runs after the re-init");
    CHECK(a52session_refresh_rate(s) / 1000 == 49, "PAL rate %d", a52session_refresh_rate(s));
    a52session_bios_name(s, name, sizeof name);
    CHECK(strstr(name, "Altirra"), "bios %s", name);
    a52session_set_int(s, "tv", 0);
    a52session_apply_settings(s);
    CHECK(test_wait_frames(s, 20), "runs again");

    /* the address text pref */
    {
        int ai = a52session_pref_find("fujinet_external_addr");
        char err[100];
        CHECK(a52session_pref_set_text(s, ai, "10.0.0.5:11524", err, sizeof err) == 0, "text ok");
        CHECK(a52session_pref_placeholder(ai) != NULL, "placeholder");
    }
    a52session_stop(s);
    a52session_free(s);
    puts("prefs: ok");
    return 0;
}
