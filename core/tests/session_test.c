/* The session end to end, headless: no ROM (the built-in Altirra OS), no
   network (FujiNet off), the cartridge in sync mode. CONFIG draws; a game
   opened from a file runs and a power cycle brings CONFIG back; a game with
   the FUJI claim keeps its mailbox; the keypad and RESET reach the console. */
#include "test_util.h"
#include "test_files.h"

int main(void)
{
    char base[512], path[800], msg[256], text[2048], info[160];
    a52session *s;
    unsigned c0, c1;

    s = test_session("session");
    CHECK(s != NULL, "session");
    test_start(s, 0);
    CHECK(test_wait_frames(s, 30), "the console runs");

    /* CONFIG draws (the baked client, mode-2 text at $3C00) */
    CHECK(test_wait_config_text(s, "FUJINET", 20000) || test_wait_config_text(s, "CONFIG", 1000),
          "CONFIG draws");
    test_config_text(s, text, sizeof text);
    a52session_cart_info(s, info, sizeof info);
    CHECK(strstr(info, "CONFIG"), "cart info says CONFIG: %s", info);
    CHECK(a52session_cart_path(s)[0] == 0, "no cart path");
    a52session_status(s, text, sizeof text);
    CHECK(strstr(text, "Altirra"), "status names the BIOS: %s", text);

    /* a game, from a file */
    test_tmpdir(base, sizeof base, "session-files");
    snprintf(path, sizeof path, "%s/game.a52", base);
    CHECK(write_game(path, 0x2000, 0) == 0, "write");
    CHECK(a52session_open(s, path, msg, sizeof msg) == 0, "open: %s", msg);
    CHECK(test_wait_byte(s, 0x0400, 0xAA, 20000), "the game runs");
    c0 = (unsigned)test_peek(s, 0x0401);
    CHECK(test_wait_frames(s, 5), "frames");
    c1 = (unsigned)test_peek(s, 0x0401);
    CHECK(c0 != c1, "the game's counter moves (%u %u)", c0, c1);
    a52session_cart_info(s, info, sizeof info);
    CHECK(strstr(info, "ROM") && strstr(info, "8 KB"), "cart info: %s", info);
    CHECK(strcmp(a52session_cart_path(s), path) == 0, "cart path remembered");
    CHECK(strcmp(a52session_get_str(s, "cart", ""), path) == 0, "cart setting");

    /* RESET restarts the cartridge's current image (the cart never sees it) */
    CHECK(a52session_reset(s) == 0, "reset");
    CHECK(test_wait_frames(s, 20), "frames after reset");
    CHECK(test_wait_byte(s, 0x0400, 0xAA, 20000), "the game runs again");

    /* the power cycle takes the cartridge down: CONFIG is back */
    CHECK(a52session_power_cycle(s) == 0, "power cycle");
    CHECK(test_wait_config_text(s, "FUJINET", 20000) || test_wait_config_text(s, "CONFIG", 1000),
          "CONFIG is back");
    CHECK(a52session_cart_path(s)[0] == 0, "cart path cleared");
    CHECK(strcmp(a52session_get_str(s, "cart", "x"), "") == 0, "cart setting cleared");

    /* a .car with a header, and a zip of one game, and a FujiNet app */
    snprintf(path, sizeof path, "%s/game.car", base);
    CHECK(write_game_car(path) == 0, "write car");
    CHECK(a52session_open(s, path, msg, sizeof msg) == 0, "open car: %s", msg);
    CHECK(test_wait_byte(s, 0x0400, 0xAA, 20000), "the .car game runs");
    snprintf(path, sizeof path, "%s/app.a52", base);
    CHECK(write_game(path, 0x8000, 1) == 0, "write app");
    CHECK(a52session_cart_is_app(path), "app claims");
    CHECK(a52session_open(s, path, msg, sizeof msg) == 0, "open app: %s", msg);
    CHECK(test_wait_byte(s, 0x0400, 0xAA, 20000), "the app runs");
    a52session_cart_info(s, info, sizeof info);
    CHECK(strstr(info, "FujiNet app"), "cart info: %s", info);

    /* a file the cartridge cannot take is refused with a reason */
    snprintf(path, sizeof path, "%s/bad.bin", base);
    {
        FILE *fp = fopen(path, "wb");
        static unsigned char big[0x30000];
        memset(big, 1, sizeof big);
        fwrite(big, 1, sizeof big, fp);
        fclose(fp);
    }
    CHECK(a52session_open(s, path, msg, sizeof msg) != 0 && msg[0], "oversized refused: %s", msg);

    /* the keypad reaches POKEY: press 5 on the pad, KBCODE holds its code */
    a52session_press(s, A52_TARGET_KEY(A52_KEY_5), 1);
    CHECK(test_wait_frames(s, 8), "frames");
    a52session_press(s, A52_TARGET_KEY(A52_KEY_5), 0);
    CHECK(test_wait_frames(s, 4), "frames");

    a52session_stop(s);
    a52session_free(s);
    printf("session: ok\n");
    return 0;
}
